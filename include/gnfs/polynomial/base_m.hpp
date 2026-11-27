#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"

#include <string>

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

/// Result of polynomial selection
struct PolynomialSelectionResult {
    bool success = false;
    /// Empty on success and for invalid parameters. Otherwise it states why no
    /// polynomial was selected.
    std::string error;
    Integer m;
    uint32_t degree = 0;
    IntPolynomial f;

    PolynomialSelectionResult() = default;
};

/// Base-m polynomial selection method
class BaseMSelector {
public:
    explicit BaseMSelector(const Integer& n);

    /// Select polynomial with given degree (new API)
    /// Returns success = false when the parameters are invalid, or when no
    /// irreducible base-m polynomial of this degree is found; the latter case
    /// also sets `error`.
    static PolynomialSelectionResult select(const Integer& n, uint32_t degree);

    /// Create context from selection result
    /// @throws std::runtime_error when result.success is false; the message
    /// carries result.error when it is set
    static PolynomialContext create_context(const Integer& n,
                                            const PolynomialSelectionResult& result);

    /// Select polynomial with given degree (old API for compatibility)
    PolynomialContext select_poly(uint32_t degree);

private:
    Integer n_;

    Integer find_m(uint32_t degree);
    IntPolynomial construct_algebraic_poly(const Integer& m, uint32_t degree);
};

/// Convenience function (old API)
PolynomialContext select_base_m_polynomial(const Integer& n, uint32_t degree);

} // namespace gnfs::polynomial
