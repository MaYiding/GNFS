#pragma once

// Pure helpers behind the diagnostics that tests/test_gnfs_e2e_ladder.cpp
// writes into its record: the classification of the selected polynomial over
// F2, the classification of the square-root failure logs, the comparison of
// two polynomial selections, and the UTF-8 cut of a message sample. The
// ladder binary runs only nightly and asserts nothing about them, so
// tests/test_ladder_diagnostics.cpp checks them on every pull request.

#include <gnfs/api/progress.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::tests::support {

// ============================================================
// Polynomials over F2
// ============================================================

// A polynomial over F2. Index i holds the coefficient of x^i, and the vector
// never ends with a zero coefficient. The zero polynomial is therefore empty,
// and every other polynomial is monic.
using F2Polynomial = std::vector<unsigned>;

struct F2Division final {
    F2Polynomial quotient;
    F2Polynomial remainder;
};

// Degree of a polynomial, or -1 for the zero polynomial.
[[nodiscard]] inline int f2_degree(const F2Polynomial& polynomial) noexcept {
    return static_cast<int>(polynomial.size()) - 1;
}

[[nodiscard]] inline F2Polynomial f2_trimmed(F2Polynomial polynomial) {
    while (!polynomial.empty() && polynomial.back() == 0U) {
        polynomial.pop_back();
    }
    return polynomial;
}

// Long division. The divisor must be nonzero.
[[nodiscard]] inline F2Division f2_divide(F2Polynomial dividend, const F2Polynomial& divisor) {
    const int divisor_degree = f2_degree(divisor);
    const int quotient_size = f2_degree(dividend) - divisor_degree + 1;
    F2Division division;
    if (quotient_size > 0) {
        division.quotient.resize(static_cast<size_t>(quotient_size), 0U);
        for (int shift = quotient_size - 1; shift >= 0; --shift) {
            const size_t base = static_cast<size_t>(shift);
            if (dividend[base + static_cast<size_t>(divisor_degree)] == 0U) {
                continue;
            }
            division.quotient[base] = 1U;
            for (size_t index = 0; index < divisor.size(); ++index) {
                dividend[base + index] ^= divisor[index];
            }
        }
    }
    division.remainder = f2_trimmed(std::move(dividend));
    return division;
}

// The gcd of two polynomials. Two zero polynomials give the zero polynomial.
[[nodiscard]] inline F2Polynomial f2_gcd(F2Polynomial a, F2Polynomial b) {
    while (!b.empty()) {
        F2Polynomial remainder = f2_divide(std::move(a), b).remainder;
        a = std::move(b);
        b = std::move(remainder);
    }
    return a;
}

// The formal derivative. Over F2 the term x^i vanishes for even i.
[[nodiscard]] inline F2Polynomial f2_derivative(const F2Polynomial& polynomial) {
    F2Polynomial derivative;
    for (size_t exponent = 1; exponent < polynomial.size(); ++exponent) {
        derivative.push_back(exponent % 2 == 1 ? polynomial[exponent] : 0U);
    }
    return f2_trimmed(std::move(derivative));
}

[[nodiscard]] inline F2Polynomial f2_product(const F2Polynomial& a, const F2Polynomial& b) {
    F2Polynomial product;
    if (a.empty() || b.empty()) {
        return product;
    }
    product.assign(a.size() + b.size(), 0U);
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == 0U) {
            continue;
        }
        for (size_t j = 0; j < b.size(); ++j) {
            product[i + j] ^= b[j];
        }
    }
    return f2_trimmed(std::move(product));
}

// The unique square root of a square in F2[x]: squaring maps the coefficient
// of x^i to the coefficient of x^(2i).
[[nodiscard]] inline F2Polynomial f2_square_root(const F2Polynomial& square) {
    F2Polynomial root;
    for (size_t exponent = 0; exponent < square.size(); exponent += 2) {
        root.push_back(square[exponent]);
    }
    return f2_trimmed(std::move(root));
}

// `polynomial` + x.
[[nodiscard]] inline F2Polynomial f2_plus_x(F2Polynomial polynomial) {
    if (polynomial.size() < 2) {
        polynomial.resize(2, 0U);
    }
    polynomial[1] ^= 1U;
    return f2_trimmed(std::move(polynomial));
}

struct F2SquarefreeFactor final {
    // Squarefree, but possibly a product of irreducibles that share a
    // multiplicity.
    F2Polynomial polynomial;
    size_t multiplicity = 0;
};

// Squarefree factorization of a polynomial of degree at least one. The
// factors are squarefree and pairwise coprime, and their powers rebuild the
// input. Each round separates the factors of odd multiplicity, and the rest
// is a square whose root goes through the next round with doubled weights.
inline void f2_collect_squarefree_factors(const F2Polynomial& polynomial, size_t multiplicity_unit,
                                          std::vector<F2SquarefreeFactor>& factors) {
    const F2Polynomial common = f2_gcd(polynomial, f2_derivative(polynomial));
    F2Polynomial odd_part = f2_divide(polynomial, common).quotient;
    F2Polynomial even_part = common;
    size_t multiplicity = 1;
    while (f2_degree(odd_part) > 0) {
        const F2Polynomial shared = f2_gcd(odd_part, even_part);
        const F2Polynomial factor = f2_divide(odd_part, shared).quotient;
        if (f2_degree(factor) > 0) {
            factors.push_back(F2SquarefreeFactor{factor, multiplicity * multiplicity_unit});
        }
        odd_part = shared;
        even_part = f2_divide(even_part, shared).quotient;
        ++multiplicity;
    }
    if (f2_degree(even_part) > 0) {
        f2_collect_squarefree_factors(f2_square_root(even_part), multiplicity_unit * 2, factors);
    }
}

// Multiplicity of `linear` in `polynomial`.
[[nodiscard]] inline size_t f2_multiplicity_of(F2Polynomial polynomial,
                                               const F2Polynomial& linear) {
    size_t multiplicity = 0;
    while (f2_degree(polynomial) >= 1) {
        F2Division division = f2_divide(std::move(polynomial), linear);
        if (!division.remainder.empty()) {
            break;
        }
        polynomial = std::move(division.quotient);
        ++multiplicity;
    }
    return multiplicity;
}

// Rabin's test: a polynomial of degree n is irreducible when x^(2^n) is x
// modulo the polynomial and x lies in no proper subfield, which the prime
// divisors of n generate.
[[nodiscard]] inline bool f2_is_irreducible(const F2Polynomial& polynomial) {
    const int degree = f2_degree(polynomial);
    if (degree <= 0) {
        return false;
    }
    if (degree == 1) {
        return true;
    }
    // frobenius[k] is x^(2^k) modulo the polynomial.
    std::vector<F2Polynomial> frobenius(static_cast<size_t>(degree) + 1);
    frobenius[0] = {0U, 1U};
    for (int k = 1; k <= degree; ++k) {
        const F2Polynomial& previous = frobenius[static_cast<size_t>(k) - 1];
        frobenius[static_cast<size_t>(k)] =
            f2_divide(f2_product(previous, previous), polynomial).remainder;
    }
    if (!f2_plus_x(frobenius[static_cast<size_t>(degree)]).empty()) {
        return false;
    }
    // The smallest divisor above one of the remaining degree is prime, so
    // every candidate that divides it is one of its prime divisors.
    int remaining = degree;
    for (int candidate = 2; candidate <= remaining; ++candidate) {
        if (remaining % candidate != 0) {
            continue;
        }
        const size_t subfield_degree = static_cast<size_t>(degree / candidate);
        if (f2_degree(f2_gcd(f2_plus_x(frobenius[subfield_degree]), polynomial)) != 0) {
            return false;
        }
        while (remaining % candidate == 0) {
            remaining /= candidate;
        }
    }
    return true;
}

// The properties of a polynomial of degree at least one over F2.
struct F2PolynomialType final {
    // No irreducible factor appears more than once.
    bool squarefree = false;
    // The factors x and x + 1, counted with multiplicity.
    size_t linear_factors = 0;
    // The polynomial is the power of a polynomial of lower degree.
    bool perfect_power = false;
    bool irreducible = false;

    [[nodiscard]] bool operator==(const F2PolynomialType&) const = default;
};

// Classifies a polynomial of degree at least one over F2.
[[nodiscard]] inline F2PolynomialType f2_classify(const F2Polynomial& polynomial) {
    F2PolynomialType type;
    std::vector<F2SquarefreeFactor> factors;
    f2_collect_squarefree_factors(polynomial, 1, factors);
    bool squarefree = true;
    size_t exponent_gcd = 0;
    for (const F2SquarefreeFactor& factor : factors) {
        squarefree = squarefree && factor.multiplicity == 1;
        exponent_gcd = std::gcd(exponent_gcd, factor.multiplicity);
    }
    type.squarefree = squarefree;
    const size_t multiplicity_of_x = f2_multiplicity_of(polynomial, F2Polynomial{0U, 1U});
    const size_t multiplicity_of_x_plus_one = f2_multiplicity_of(polynomial, F2Polynomial{1U, 1U});
    type.linear_factors = multiplicity_of_x + multiplicity_of_x_plus_one;
    // A power h^k of a polynomial of lower degree exists exactly when every
    // multiplicity is divisible by k.
    type.perfect_power = exponent_gcd >= 2;
    type.irreducible = factors.size() == 1 && factors.front().multiplicity == 1 &&
                       f2_is_irreducible(factors.front().polynomial);
    return type;
}

// ============================================================
// Failure logs of the square root stage
// ============================================================

// Message texts of the square root stage. The pipeline logs a failed rational
// or algebraic square root as a Warn whose text starts with the failure name
// and ends with the reason that the square root code returned.
constexpr std::string_view kAlgebraicSqrtFailedPrefix = "Algebraic sqrt failed";
constexpr std::string_view kRationalSqrtFailedPrefix = "Rational sqrt failed";
// The parity pre-check of the algebraic square root runs before Hensel
// lifting and rejects the dependency without either square root algorithm.
constexpr std::string_view kIdealParityReason = "Algebraic ideal powers parity check failed";
// The rational square root rejects an odd prime exponent for the same reason:
// the selected relations do not multiply to a square.
constexpr std::string_view kRationalFactorBaseParityReason = "Factor base exponent not even";
constexpr std::string_view kRationalLargePrimeParityReason = "Large prime exponent not even";
// The algebraic square root reaches Couveignes only after Hensel lifting
// returned no root, unless GNFS_FORCE_COUVEIGNES=1 skips Hensel.
constexpr std::string_view kCouveignesFailedReason = "Couveignes algorithm failed";
constexpr std::string_view kHenselName = "Hensel";

// Categories of the Warn and Error messages that the square root stage
// emits. One message can fall into several categories: an algebraic failure
// also counts under its reason, and a Couveignes failure counts as a Hensel
// failure as well unless the process forces the Couveignes path.
enum class SqrtFailureCategory : uint8_t {
    AlgebraicSqrtFailed,
    RationalSqrtFailed,
    IdealParityPrecheckFailed,
    RationalParityFailed,
    CouveignesFailed,
    HenselRelated,
    OtherSquareRoot,
    Count
};
constexpr size_t kSqrtFailureCategoryCount = static_cast<size_t>(SqrtFailureCategory::Count);

[[nodiscard]] constexpr std::string_view
sqrt_failure_category_name(SqrtFailureCategory category) noexcept {
    switch (category) {
    case SqrtFailureCategory::AlgebraicSqrtFailed:
        return "algebraic_sqrt_failed";
    case SqrtFailureCategory::RationalSqrtFailed:
        return "rational_sqrt_failed";
    case SqrtFailureCategory::IdealParityPrecheckFailed:
        return "ideal_parity_precheck_failed";
    case SqrtFailureCategory::RationalParityFailed:
        return "rational_parity_failed";
    case SqrtFailureCategory::CouveignesFailed:
        return "couveignes_failed";
    case SqrtFailureCategory::HenselRelated:
        return "hensel_related";
    case SqrtFailureCategory::OtherSquareRoot:
        return "other_square_root";
    case SqrtFailureCategory::Count:
        break;
    }
    return "unknown";
}

// The categories that one log entry falls into. An empty match means that
// the entry is not a failure of the square root stage.
struct SqrtFailureMatch final {
    // Indexed by SqrtFailureCategory.
    std::array<bool, kSqrtFailureCategoryCount> categories{};

    [[nodiscard]] constexpr bool any() const noexcept {
        for (bool matched : categories) {
            if (matched) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool has(SqrtFailureCategory category) const noexcept {
        return categories[static_cast<size_t>(category)];
    }

    [[nodiscard]] bool operator==(const SqrtFailureMatch&) const = default;
};

// Classifies one log entry of the pipeline. Only a Warn or an Error of the
// square root stage counts. `force_couveignes` is set when
// GNFS_FORCE_COUVEIGNES=1 makes the algebraic square root skip Hensel
// lifting, because a Couveignes failure then no longer implies that Hensel
// returned no root.
[[nodiscard]] inline SqrtFailureMatch classify_sqrt_failure_log(const gnfs::api::LogEntry& entry,
                                                                bool force_couveignes) {
    SqrtFailureMatch match;
    if (entry.level != gnfs::api::LogLevel::Warn && entry.level != gnfs::api::LogLevel::Error) {
        return match;
    }
    const std::string_view message = entry.message;
    const bool algebraic = message.starts_with(kAlgebraicSqrtFailedPrefix);
    const bool rational = message.starts_with(kRationalSqrtFailedPrefix);
    if (entry.phase != gnfs::api::Phase::SquareRoot && !algebraic && !rational) {
        return match;
    }

    const auto count = [&match](SqrtFailureCategory category) {
        match.categories[static_cast<size_t>(category)] = true;
    };
    const bool couveignes = message.find(kCouveignesFailedReason) != std::string_view::npos;
    if (algebraic) {
        count(SqrtFailureCategory::AlgebraicSqrtFailed);
    }
    if (rational) {
        count(SqrtFailureCategory::RationalSqrtFailed);
    }
    if (message.find(kIdealParityReason) != std::string_view::npos) {
        count(SqrtFailureCategory::IdealParityPrecheckFailed);
    }
    if (rational && (message.find(kRationalFactorBaseParityReason) != std::string_view::npos ||
                     message.find(kRationalLargePrimeParityReason) != std::string_view::npos)) {
        count(SqrtFailureCategory::RationalParityFailed);
    }
    if (couveignes) {
        count(SqrtFailureCategory::CouveignesFailed);
    }
    if (message.find(kHenselName) != std::string_view::npos || (couveignes && !force_couveignes)) {
        count(SqrtFailureCategory::HenselRelated);
    }
    if (!match.any()) {
        count(SqrtFailureCategory::OtherSquareRoot);
    }
    return match;
}

// ============================================================
// Comparison of two polynomial selections
// ============================================================

// The identity of one polynomial selection, as far as the record knows it.
struct PolynomialSelection final {
    // The root m, as decimal text. Disengaged when no selection produced a
    // polynomial or Pipeline::run() logged none.
    std::optional<std::string> m;
    // The degree of the integer polynomial.
    std::optional<size_t> degree;
};

// The outcome of comparing a probe polynomial with the polynomial that
// Pipeline::run() selected.
enum class PolynomialMatch : uint8_t {
    // run() logged no polynomial, so nothing confirms or contradicts the
    // probe.
    Unconfirmed,
    // Both roots and both degrees agree.
    Same,
    // run() logged another root or another degree, or the probe itself
    // selected no polynomial.
    Different
};

// run() logs only the root of its polynomial, so agreement rests on that
// root and on the degree that the published statistics name. A disengaged
// run degree, which belongs to a run that threw, skips the degree test.
[[nodiscard]] inline PolynomialMatch compare_selections(const PolynomialSelection& probe,
                                                        const PolynomialSelection& run) {
    if (!probe.m.has_value()) {
        return PolynomialMatch::Different;
    }
    if (!run.m.has_value()) {
        return PolynomialMatch::Unconfirmed;
    }
    if (*run.m != *probe.m || (run.degree.has_value() && run.degree != probe.degree)) {
        return PolynomialMatch::Different;
    }
    return PolynomialMatch::Same;
}

// ============================================================
// Message samples
// ============================================================

// Cuts a UTF-8 text to at most `max_bytes` bytes without splitting a
// multi-byte sequence.
[[nodiscard]] inline std::string_view truncate_utf8(std::string_view text,
                                                    size_t max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text;
    }
    size_t end = max_bytes;
    // A continuation byte at `end` belongs to a sequence that starts before it.
    while (end > 0) {
        const unsigned byte = static_cast<unsigned char>(text[end]);
        if ((byte & 0xc0U) != 0x80U) {
            break;
        }
        --end;
    }
    return text.substr(0, end);
}

} // namespace gnfs::tests::support
