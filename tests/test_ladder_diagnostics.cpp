// test_ladder_diagnostics.cpp — Checks for the pure helpers behind the
// diagnostics of the GNFS evidence ladder record
//
// tests/test_gnfs_e2e_ladder.cpp runs only nightly and asserts nothing about
// the diagnostics it writes, so a regression in them would change the nightly
// evidence without failing a test. This instant CTest checks the helpers in
// tests/support/ladder_diagnostics.hpp:
//
//   * The F2 arithmetic and the polynomial classification against brute-force
//     references, for every monic polynomial of degree 1 through 8.
//   * The classification of the square-root failure logs on the message texts
//     that src/api/pipeline.cpp and include/gnfs/sqrt/ emit.
//   * The attempt tracking that counts the failures the pipeline logs nothing
//     about.
//   * The comparison of two polynomial selections, the category names, and
//     the UTF-8 cut of a message sample.

#include "support/ladder_diagnostics.hpp"

#include "support/test_assert.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using gnfs::api::LogEntry;
using gnfs::api::LogLevel;
using gnfs::api::Phase;

// The helpers under test.
namespace diag = gnfs::tests::support;

namespace {

using F2Polynomial = diag::F2Polynomial;

constexpr int kMaxDegree = 8;

// ------------------------------------------------------------
// Brute-force references, independent of the helpers under test
// ------------------------------------------------------------

[[nodiscard]] F2Polynomial reference_trimmed(F2Polynomial polynomial) {
    while (!polynomial.empty() && polynomial.back() == 0U) {
        polynomial.pop_back();
    }
    return polynomial;
}

// The monic polynomials of `degree`, ascending by the bits of their lower
// coefficients.
[[nodiscard]] std::vector<F2Polynomial> monic_polynomials(int degree) {
    const size_t count = size_t{1} << degree;
    std::vector<F2Polynomial> polynomials;
    polynomials.reserve(count);
    for (size_t bits = 0; bits < count; ++bits) {
        F2Polynomial polynomial(static_cast<size_t>(degree) + 1U, 0U);
        for (int exponent = 0; exponent < degree; ++exponent) {
            const unsigned coefficient = static_cast<unsigned>((bits >> exponent) & 1U);
            polynomial[static_cast<size_t>(exponent)] = coefficient;
        }
        polynomial[static_cast<size_t>(degree)] = 1U;
        polynomials.push_back(std::move(polynomial));
    }
    return polynomials;
}

[[nodiscard]] F2Polynomial reference_product(const F2Polynomial& a, const F2Polynomial& b) {
    F2Polynomial product(a.size() + b.size() + 1U, 0U);
    for (size_t i = 0; i < a.size(); ++i) {
        for (size_t j = 0; j < b.size(); ++j) {
            product[i + j] ^= a[i] & b[j];
        }
    }
    return reference_trimmed(std::move(product));
}

struct ReferenceDivision final {
    F2Polynomial quotient;
    F2Polynomial remainder;
};

// Division by a nonzero monic divisor, from the top coefficient downwards.
[[nodiscard]] ReferenceDivision reference_divide(F2Polynomial dividend,
                                                 const F2Polynomial& divisor) {
    ReferenceDivision division;
    while (!dividend.empty() && dividend.size() >= divisor.size()) {
        const size_t shift = dividend.size() - divisor.size();
        if (division.quotient.size() <= shift) {
            division.quotient.resize(shift + 1U, 0U);
        }
        division.quotient[shift] = 1U;
        for (size_t index = 0; index < divisor.size(); ++index) {
            dividend[shift + index] ^= divisor[index];
        }
        dividend = reference_trimmed(std::move(dividend));
    }
    division.remainder = std::move(dividend);
    return division;
}

// The irreducible monic polynomials of each degree from 1 to kMaxDegree,
// found by trial division against the shorter ones.
class IrreducibleTable final {
public:
    IrreducibleTable() {
        for (int degree = 1; degree <= kMaxDegree; ++degree) {
            std::vector<F2Polynomial> found;
            for (const F2Polynomial& candidate : monic_polynomials(degree)) {
                if (is_irreducible(candidate)) {
                    found.push_back(candidate);
                }
            }
            by_degree_.push_back(std::move(found));
        }
    }

    // The irreducible polynomials of `degree`, which is at least one.
    [[nodiscard]] const std::vector<F2Polynomial>& of_degree(int degree) const {
        return by_degree_[static_cast<size_t>(degree - 1)];
    }

private:
    [[nodiscard]] bool is_irreducible(const F2Polynomial& candidate) const {
        const int degree = static_cast<int>(candidate.size()) - 1;
        for (int lower = 1; 2 * lower <= degree; ++lower) {
            for (const F2Polynomial& divisor : of_degree(lower)) {
                if (reference_divide(candidate, divisor).remainder.empty()) {
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<std::vector<F2Polynomial>> by_degree_;
};

using ReferenceFactor = std::pair<F2Polynomial, size_t>;

// Reference factorization by trial division over the table. Every
// irreducible factor of the input has a degree of at most half the degree
// of the input, except at most one that is left over.
[[nodiscard]] std::vector<ReferenceFactor> reference_factorize(F2Polynomial polynomial,
                                                               const IrreducibleTable& table) {
    std::vector<ReferenceFactor> factors;
    const int degree = static_cast<int>(polynomial.size()) - 1;
    for (int factor_degree = 1; 2 * factor_degree <= degree; ++factor_degree) {
        for (const F2Polynomial& irreducible : table.of_degree(factor_degree)) {
            if (!reference_divide(polynomial, irreducible).remainder.empty()) {
                continue;
            }
            size_t multiplicity = 0;
            while (reference_divide(polynomial, irreducible).remainder.empty()) {
                polynomial = reference_divide(std::move(polynomial), irreducible).quotient;
                ++multiplicity;
            }
            factors.emplace_back(irreducible, multiplicity);
        }
    }
    if (polynomial.size() > 1U) {
        // The rest has no factor of degree up to half the degree of the
        // input, so it is one irreducible polynomial.
        factors.emplace_back(std::move(polynomial), 1U);
    }
    return factors;
}

[[nodiscard]] size_t reference_multiplicity(const std::vector<ReferenceFactor>& factors,
                                            const F2Polynomial& irreducible) {
    for (const ReferenceFactor& factor : factors) {
        if (factor.first == irreducible) {
            return factor.second;
        }
    }
    return 0;
}

[[nodiscard]] F2Polynomial reference_power(const F2Polynomial& base, size_t exponent) {
    F2Polynomial power{1U};
    for (size_t index = 0; index < exponent; ++index) {
        power = reference_product(power, base);
    }
    return power;
}

// A power h^k of a polynomial of lower degree exists or not: try every k that
// divides the degree and every monic base of degree n / k.
[[nodiscard]] bool reference_is_perfect_power(const F2Polynomial& polynomial) {
    const int degree = static_cast<int>(polynomial.size()) - 1;
    for (int exponent = 2; exponent <= degree; ++exponent) {
        if (degree % exponent != 0) {
            continue;
        }
        for (const F2Polynomial& base : monic_polynomials(degree / exponent)) {
            if (reference_power(base, static_cast<size_t>(exponent)) == polynomial) {
                return true;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------
// Polynomials over F2
// ------------------------------------------------------------

void test_f2_classification_examples() {
    using Type = diag::F2PolynomialType;
    // x^3 + x + 1 has no root and is irreducible.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 1U, 0U, 1U}) == Type{true, 0, false, true});
    // (x + 1)^3 = x^3 + x^2 + x + 1.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 1U, 1U, 1U}) == Type{false, 3, true, false});
    // (x + 1)^4 = x^4 + 1.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 0U, 0U, 0U, 1U}) == Type{false, 4, true, false});
    // x^2 + x = x (x + 1) is squarefree and reducible.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{0U, 1U, 1U}) == Type{true, 2, false, false});
    // x^3 + 1 = (x + 1)(x^2 + x + 1).
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 0U, 0U, 1U}) == Type{true, 1, false, false});
    // x^3 + x^2 = x^2 (x + 1) is not a perfect power.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{0U, 0U, 1U, 1U}) == Type{false, 3, false, false});
    // x^4 + x^2 + 1 = (x^2 + x + 1)^2.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 0U, 1U, 0U, 1U}) == Type{false, 0, true, false});
    // x^2 + x + 1 and x^4 + x + 1 are irreducible.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 1U, 1U}) == Type{true, 0, false, true});
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 1U, 0U, 0U, 1U}) == Type{true, 0, false, true});
    // The linear polynomials.
    TEST_ASSERT(diag::f2_classify(F2Polynomial{0U, 1U}) == Type{true, 1, false, true});
    TEST_ASSERT(diag::f2_classify(F2Polynomial{1U, 1U}) == Type{true, 1, false, true});

    // Division, gcd, and the derivative on the same examples.
    const F2Division exact = diag::f2_divide(F2Polynomial{1U, 1U, 0U, 1U}, F2Polynomial{1U, 1U});
    TEST_ASSERT(exact.remainder == F2Polynomial{1U});
    TEST_ASSERT(exact.quotient == F2Polynomial{0U, 1U, 1U});
    const F2Division square = diag::f2_divide(F2Polynomial{1U, 1U, 1U, 1U}, F2Polynomial{1U, 1U});
    TEST_ASSERT(square.remainder.empty());
    TEST_ASSERT(square.quotient == F2Polynomial{1U, 0U, 1U});
    // x^2 + 1 is (x + 1)^2 and divides (x + 1)^3; x^3 + x + 1 is coprime
    // with x^2 + x, and the gcd of coprime polynomials is the constant one.
    TEST_ASSERT(diag::f2_gcd(F2Polynomial{1U, 1U, 1U, 1U}, F2Polynomial{1U, 0U, 1U}) ==
                F2Polynomial{1U, 0U, 1U});
    TEST_ASSERT(diag::f2_gcd(F2Polynomial{1U, 1U, 0U, 1U}, F2Polynomial{0U, 1U, 1U}) ==
                F2Polynomial{1U});
    TEST_ASSERT(diag::f2_derivative(F2Polynomial{1U, 1U, 1U, 1U}) == F2Polynomial{1U, 0U, 1U});
    TEST_ASSERT(diag::f2_derivative(F2Polynomial{1U, 0U, 1U, 0U, 1U}).empty());
    std::cout << "  F2 classification examples: PASS" << std::endl;
}

// Every monic polynomial of degree 1 through kMaxDegree: the classification
// and the squarefree factorization agree with the references.
void test_f2_classification_matches_references() {
    const IrreducibleTable table;
    size_t checked = 0;
    for (int degree = 1; degree <= kMaxDegree; ++degree) {
        for (const F2Polynomial& polynomial : monic_polynomials(degree)) {
            const std::vector<ReferenceFactor> reference = reference_factorize(polynomial, table);

            const diag::F2PolynomialType type = diag::f2_classify(polynomial);
            bool squarefree = true;
            size_t linear_factors = 0;
            for (const ReferenceFactor& factor : reference) {
                squarefree = squarefree && factor.second == 1;
                if (factor.first == F2Polynomial{0U, 1U} || factor.first == F2Polynomial{1U, 1U}) {
                    linear_factors += factor.second;
                }
            }
            TEST_ASSERT(type.squarefree == squarefree);
            TEST_ASSERT(type.linear_factors == linear_factors);
            TEST_ASSERT(type.perfect_power == reference_is_perfect_power(polynomial));
            const bool irreducible = reference.size() == 1 && reference.front().second == 1;
            TEST_ASSERT(type.irreducible == irreducible);

            std::vector<diag::F2SquarefreeFactor> factors;
            diag::f2_collect_squarefree_factors(polynomial, size_t{1}, factors);
            F2Polynomial rebuilt{1U};
            for (const diag::F2SquarefreeFactor& factor : factors) {
                // A squarefree factor is squarefree, and every irreducible
                // polynomial in it has the multiplicity of the factor.
                const std::vector<ReferenceFactor> inner =
                    reference_factorize(factor.polynomial, table);
                TEST_ASSERT(!inner.empty());
                for (const ReferenceFactor& component : inner) {
                    TEST_ASSERT(component.second == 1);
                    TEST_ASSERT(reference_multiplicity(reference, component.first) ==
                                factor.multiplicity);
                }
                rebuilt = reference_product(
                    rebuilt, reference_power(factor.polynomial, factor.multiplicity));
            }
            // The powers of the factors rebuild the polynomial. With the
            // multiplicities above, the factors then cover the polynomial
            // exactly and share no irreducible polynomial.
            TEST_ASSERT(rebuilt == polynomial);
            ++checked;
        }
    }
    // Degrees 1 through kMaxDegree hold 2 + 4 + ... + 2^kMaxDegree monic
    // polynomials.
    TEST_ASSERT(checked == (size_t{1} << (kMaxDegree + 1)) - 2U);
    std::cout << "  Classified " << checked << " monic polynomials: PASS" << std::endl;
}

// ------------------------------------------------------------
// Failure logs of the square root stage
// ------------------------------------------------------------

[[nodiscard]] LogEntry make_log(LogLevel level, Phase phase, std::string message) {
    LogEntry entry;
    entry.level = level;
    entry.phase = phase;
    entry.timestamp_s = 0.0;
    entry.message = std::move(message);
    return entry;
}

[[nodiscard]] diag::SqrtFailureMatch classify_warn(std::string_view message,
                                                   bool force_couveignes = false) {
    return diag::classify_sqrt_failure_log(
        make_log(LogLevel::Warn, Phase::SquareRoot, std::string(message)), force_couveignes);
}

[[nodiscard]] size_t matched_count(const diag::SqrtFailureMatch& match) {
    size_t count = 0;
    for (bool matched : match.categories) {
        count += matched ? 1U : 0U;
    }
    return count;
}

// The match holds exactly the expected categories and no other one.
void expect_match(const diag::SqrtFailureMatch& match,
                  std::initializer_list<diag::SqrtFailureCategory> expected) {
    for (const diag::SqrtFailureCategory category : expected) {
        TEST_ASSERT(match.has(category));
    }
    TEST_ASSERT(matched_count(match) == expected.size());
}

void test_sqrt_failure_log_classification() {
    // A short name keeps the expected category lists on one line.
    using Cat = diag::SqrtFailureCategory;
    // The texts come from src/api/pipeline.cpp and include/gnfs/sqrt/.
    const std::string algebraic =
        "Algebraic sqrt failed for dependency 1/5, using algebraic value 1 instead: ";
    const std::string rational = "Rational sqrt failed for dependency 2/5: ";
    const std::string couveignes_text = algebraic + "Couveignes algorithm failed";
    const std::string negative_text =
        rational + "Rational sqrt: signed relation product is negative";

    const auto parity = classify_warn(algebraic + "Algebraic ideal powers parity check failed");
    expect_match(parity, {Cat::AlgebraicSqrtFailed, Cat::IdealParityPrecheckFailed});
    const auto no_relations = classify_warn(algebraic + "No relations in dependency");
    expect_match(no_relations, {Cat::AlgebraicSqrtFailed});
    // Couveignes runs only after Hensel lifting returned no root, unless the
    // process forces the Couveignes path.
    const auto couveignes = classify_warn(couveignes_text);
    expect_match(couveignes, {Cat::AlgebraicSqrtFailed, Cat::CouveignesFailed, Cat::HenselRelated});
    const auto forced = classify_warn(couveignes_text, true);
    expect_match(forced, {Cat::AlgebraicSqrtFailed, Cat::CouveignesFailed});
    const auto hensel = classify_warn(algebraic + "Hensel lifting exhausted");
    expect_match(hensel, {Cat::AlgebraicSqrtFailed, Cat::HenselRelated});
    const auto factor_base_parity = classify_warn(rational + "Factor base exponent not even");
    expect_match(factor_base_parity, {Cat::RationalSqrtFailed, Cat::RationalParityFailed});
    const auto large_prime_parity = classify_warn(rational + "Large prime exponent not even");
    expect_match(large_prime_parity, {Cat::RationalSqrtFailed, Cat::RationalParityFailed});
    const auto negative = classify_warn(negative_text);
    expect_match(negative, {Cat::RationalSqrtFailed});

    // A square-root Warn or Error that names no failure falls into the other
    // bucket, and the two failure signatures count in any phase.
    const std::string none = "No dependencies to try";
    const LogEntry none_entry = make_log(LogLevel::Error, Phase::SquareRoot, none);
    expect_match(diag::classify_sqrt_failure_log(none_entry, false), {Cat::OtherSquareRoot});
    const LogEntry late_entry = make_log(LogLevel::Warn, Phase::FactorExtraction, couveignes_text);
    const auto late = diag::classify_sqrt_failure_log(late_entry, false);
    expect_match(late, {Cat::AlgebraicSqrtFailed, Cat::CouveignesFailed, Cat::HenselRelated});

    // Other levels, other phases, and other texts of the square root stage
    // are not failures.
    const auto started = classify_warn("Starting factor extraction");
    TEST_ASSERT(!started.any());
    const LogEntry info_entry = make_log(LogLevel::Info, Phase::SquareRoot, negative_text);
    const auto info_level = diag::classify_sqrt_failure_log(info_entry, false);
    TEST_ASSERT(!info_level.any());
    const std::string no_factor = "No non-trivial factor found";
    const LogEntry outcome_entry = make_log(LogLevel::Warn, Phase::FactorExtraction, no_factor);
    const auto outcome = diag::classify_sqrt_failure_log(outcome_entry, false);
    TEST_ASSERT(!outcome.any());
    const std::string dropped = "Dropped dependency 3/5: not a null vector of the 10x9 matrix";
    const LogEntry dropped_entry = make_log(LogLevel::Warn, Phase::LinearAlgebra, dropped);
    const auto dropped_match = diag::classify_sqrt_failure_log(dropped_entry, false);
    TEST_ASSERT(!dropped_match.any());
    std::cout << "  Square-root failure log classification: PASS" << std::endl;
}

void test_sqrt_failure_category_names() {
    std::vector<std::string_view> names;
    for (size_t index = 0; index < diag::kSqrtFailureCategoryCount; ++index) {
        const auto category = static_cast<diag::SqrtFailureCategory>(index);
        const std::string_view name = diag::sqrt_failure_category_name(category);
        TEST_ASSERT(name != "unknown");
        TEST_ASSERT(std::find(names.begin(), names.end(), name) == names.end());
        names.push_back(name);
    }
    TEST_ASSERT(diag::sqrt_failure_category_name(diag::SqrtFailureCategory::UnloggedFailure) ==
                "unlogged_failure");
    std::cout << "  Square-root failure category names: PASS" << std::endl;
}

// ------------------------------------------------------------
// Attempts of the factor extraction
// ------------------------------------------------------------

[[nodiscard]] std::string single_dependency(int index) {
    return "Trying dependency " + std::to_string(index) + "/5";
}

[[nodiscard]] std::string xor_dependency(int number) {
    return "Trying XOR dependency " + std::to_string(number);
}

void test_attempt_tracker() {
    using Tracker = diag::SqrtAttemptTracker;

    // A silent failure, then a success.
    {
        Tracker tracker;
        tracker.observe_progress("Starting factor extraction");
        tracker.observe_progress(single_dependency(1));
        tracker.observe_progress(single_dependency(2));
        tracker.observe_progress("Factorization succeeded");
        tracker.finish(true);
        TEST_ASSERT(tracker.unlogged_failures() == 1);
        TEST_ASSERT(tracker.single_dependencies_tried() == 2);
        TEST_ASSERT(!tracker.pairwise_xor_attempted());
        TEST_ASSERT(tracker.pairwise_xor_dependencies_tried() == 0);
    }
    // Four logged failures, then a success on the fifth dependency.
    {
        Tracker tracker;
        for (int index = 1; index <= 4; ++index) {
            tracker.observe_progress(single_dependency(index));
            tracker.note_failure_log();
        }
        tracker.observe_progress(single_dependency(5));
        tracker.finish(true);
        TEST_ASSERT(tracker.unlogged_failures() == 0);
        TEST_ASSERT(tracker.single_dependencies_tried() == 5);
        TEST_ASSERT(!tracker.pairwise_xor_attempted());
    }
    // Dependencies 1 and 3 fail silently, 2 and 4 log, and 5 succeeds.
    {
        Tracker tracker;
        tracker.observe_progress(single_dependency(1));
        tracker.observe_progress(single_dependency(2));
        tracker.note_failure_log();
        tracker.observe_progress(single_dependency(3));
        tracker.observe_progress(single_dependency(4));
        tracker.note_failure_log();
        tracker.observe_progress(single_dependency(5));
        tracker.finish(true);
        TEST_ASSERT(tracker.unlogged_failures() == 2);
    }
    // Every single dependency fails, the XOR fallback runs and fails, and the
    // run ends without factors. Dependency 1, dependency 3, and pair 5 are
    // silent.
    {
        Tracker tracker;
        tracker.observe_progress(single_dependency(1));
        tracker.observe_progress(single_dependency(2));
        tracker.note_failure_log();
        tracker.observe_progress(single_dependency(3));
        tracker.observe_progress("Trying XOR combinations");
        tracker.observe_progress(xor_dependency(4));
        tracker.note_failure_log();
        tracker.observe_progress(xor_dependency(5));
        tracker.observe_progress(xor_dependency(6));
        tracker.note_failure_log();
        tracker.observe_progress("No non-trivial factor found");
        tracker.finish(false);
        TEST_ASSERT(tracker.unlogged_failures() == 3);
        TEST_ASSERT(tracker.single_dependencies_tried() == 3);
        TEST_ASSERT(tracker.pairwise_xor_attempted());
        TEST_ASSERT(tracker.pairwise_xor_dependencies_tried() == 3);
    }
    // The same run, but the seventh pair succeeds. Pair 6 is silent too.
    {
        Tracker tracker;
        tracker.observe_progress(single_dependency(1));
        tracker.observe_progress(single_dependency(2));
        tracker.note_failure_log();
        tracker.observe_progress(single_dependency(3));
        tracker.observe_progress("Trying XOR combinations");
        tracker.observe_progress(xor_dependency(4));
        tracker.note_failure_log();
        tracker.observe_progress(xor_dependency(5));
        tracker.observe_progress(xor_dependency(6));
        tracker.observe_progress(xor_dependency(7));
        tracker.observe_progress("Factorization succeeded");
        tracker.finish(true);
        TEST_ASSERT(tracker.unlogged_failures() == 4);
        TEST_ASSERT(tracker.pairwise_xor_dependencies_tried() == 4);
    }
    // One dependency fails silently, and fewer than two dependencies exist,
    // so the fallback never starts.
    {
        Tracker tracker;
        tracker.observe_progress(single_dependency(1));
        tracker.observe_progress("No non-trivial factor found");
        tracker.finish(false);
        TEST_ASSERT(tracker.unlogged_failures() == 1);
        TEST_ASSERT(tracker.single_dependencies_tried() == 1);
        TEST_ASSERT(!tracker.pairwise_xor_attempted());
    }
    // Unrelated progress messages change nothing, and a run that throws
    // leaves its open attempt unresolved.
    {
        Tracker tracker;
        tracker.observe_progress("SGE preprocessing");
        tracker.observe_progress("Trying SIQS for 30-digit N");
        tracker.finish(false);
        TEST_ASSERT(tracker.unlogged_failures() == 0);
        TEST_ASSERT(tracker.single_dependencies_tried() == 0);
    }
    {
        Tracker tracker;
        tracker.observe_progress(single_dependency(1));
        TEST_ASSERT(tracker.unlogged_failures() == 0);
    }
    std::cout << "  Attempt tracking: PASS" << std::endl;
}

// ------------------------------------------------------------
// Comparison of two polynomial selections, and message samples
// ------------------------------------------------------------

void test_selection_comparison() {
    using Match = diag::PolynomialMatch;
    diag::PolynomialSelection probe;
    probe.m = "123456789";
    probe.degree = size_t{3};
    diag::PolynomialSelection run_selection = probe;
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Same);

    // A run that threw publishes no degree; the root alone confirms.
    run_selection.degree.reset();
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Same);

    // Another degree or another root.
    run_selection.degree = size_t{4};
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Different);
    run_selection = probe;
    run_selection.m = "987654321";
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Different);

    // run() logged no polynomial, and the probe itself failed.
    run_selection.m.reset();
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Unconfirmed);
    probe.m.reset();
    TEST_ASSERT(diag::compare_selections(probe, run_selection) == Match::Different);
    std::cout << "  Selection comparison: PASS" << std::endl;
}

void test_truncate_utf8() {
    TEST_ASSERT(diag::truncate_utf8("abcde", 3) == "abc");
    TEST_ASSERT(diag::truncate_utf8("abcde", 5) == "abcde");
    TEST_ASSERT(diag::truncate_utf8("abcde", 9) == "abcde");
    TEST_ASSERT(diag::truncate_utf8("", 3).empty());
    // "a", the two-byte character U+00E9, and "b". A cut inside the
    // character backs off to the last whole one.
    const std::string text = std::string("a") + "\xC3\xA9" + "b";
    TEST_ASSERT(diag::truncate_utf8(text, 1) == "a");
    TEST_ASSERT(diag::truncate_utf8(text, 2) == "a");
    TEST_ASSERT(diag::truncate_utf8(text, 3) == "a\xC3\xA9");
    TEST_ASSERT(diag::truncate_utf8(text, 4) == text);
    std::cout << "  UTF-8 sample cut: PASS" << std::endl;
}

} // namespace

int main() {
    int failed = 0;

    std::cout << "=== Ladder Diagnostics Tests ===" << std::endl;

    failed += !gnfs::test::run_case(test_sqrt_failure_category_names);
    failed += !gnfs::test::run_case(test_f2_classification_examples);
    failed += !gnfs::test::run_case(test_f2_classification_matches_references);
    failed += !gnfs::test::run_case(test_sqrt_failure_log_classification);
    failed += !gnfs::test::run_case(test_attempt_tracker);
    failed += !gnfs::test::run_case(test_selection_comparison);
    failed += !gnfs::test::run_case(test_truncate_utf8);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << std::endl << "=== All Ladder Diagnostics Tests PASSED ===" << std::endl;
    return 0;
}
