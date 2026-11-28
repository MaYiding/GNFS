// test_schirokauer_precision_e2e.cpp — Production GNFS factorization of a
// semiprime whose selected polynomial has a linear factor modulo 2
//
// The Schirokauer column of a linear factor is bit v+1 of a - b·r with
// v = v₂(a - b·r), and the map can produce that bit only while v + 2 ≤ k.
// With the former default k = 8 every relation with v ≥ 7 received a zero
// column in place of its character, so about half of those relations carried
// a wrong constraint and cost a square-root attempt. This test runs the
// complete pipeline with the production defaults and requires zero precision
// fallbacks.
//
// Fixture: N = 1869045589843040925386929 = 1309931996651 × 1426826426579, a
// 25-digit semiprime. BaseMSelector selects the base-m polynomial
// f = x^3 + 148x^2 + 24945452x + 19508769 with m = 123179884, and
// f ≡ x^3 + 1 = (x + 1)(x^2 + x + 1) (mod 2). The exact columns of that
// linear factor are pinned in tests/test_schirokauer_precision.cpp.
//
// The flow copies tests/test_gnfs_e2e_ladder.cpp. Pipeline::run() is forced
// with Config::set_method(FactorizationMethod::GNFS), and the isolation set of
// the ladder keeps resume, distributed workers, and the route switches off.
// GNFS_DISABLE_SIQS=1 also removes the 30-second SIQS probe, so the factors
// can only come from the GNFS phases. The degenerate-row and precision-fallback
// counts are read from the log records of the matrix phase.

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <gnfs/api/pipeline.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/linalg/schirokauer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using gnfs::api::Config;
using gnfs::api::FactorizationMethod;
using gnfs::api::FactorResult;
using gnfs::api::FactorStats;
using gnfs::api::LogEntry;
using gnfs::api::LogLevel;
using gnfs::api::Phase;
using gnfs::api::Pipeline;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::linalg::SchirokaurMap;
using gnfs::tests::support::ScopedEnvironmentVariable;

namespace {

constexpr std::string_view kN = "1869045589843040925386929";
constexpr std::string_view kExpectedP = "1309931996651";
constexpr std::string_view kExpectedQ = "1426826426579";

// Log record prefixes of the pipeline that this test reads.
constexpr std::string_view kPolynomialPrefix = "m=";
constexpr std::string_view kDegeneratePrefix = "schirokauer degenerate rows";
constexpr std::string_view kFallbackPrefix = "schirokauer precision fallbacks";
// The Warn record starts with a capital letter; the Info records above do not.
constexpr std::string_view kFallbackWarningPrefix = "Schirokauer precision fallbacks";

// One "<value>/<total>" record: a count over the number of matrix rows.
struct Count final {
    size_t value = 0;
    size_t total = 0;
};

// The log records of one run that this test reads.
struct RunEvidence final {
    std::optional<std::string> polynomial_m;
    std::optional<Count> degenerate_rows;
    std::optional<Count> precision_fallbacks;
    size_t precision_fallback_warnings = 0;
};

std::optional<size_t> parse_digits(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const size_t digit = static_cast<size_t>(character - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

// Parse "<value>/<total>" after any leading spaces.
std::optional<Count> parse_count(std::string_view text) {
    const size_t start = text.find_first_not_of(' ');
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    text = text.substr(start);
    const size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<size_t> value = parse_digits(text.substr(0, slash));
    const std::optional<size_t> total = parse_digits(text.substr(slash + 1));
    if (!value.has_value() || !total.has_value()) {
        return std::nullopt;
    }
    return Count{*value, *total};
}

std::optional<std::string> parse_leading_digits(std::string_view text) {
    size_t end = 0;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
    }
    if (end == 0) {
        return std::nullopt;
    }
    return std::string(text.substr(0, end));
}

// Reads the log records of one run. The pipeline may log from more than one
// thread, so every access is serialized.
class EvidenceRecorder final {
public:
    void observe(const LogEntry& entry) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string_view message(entry.message);
        if (entry.level == LogLevel::Warn && message.starts_with(kFallbackWarningPrefix)) {
            ++fallback_warnings_;
            return;
        }
        if (entry.phase == Phase::PolynomialSelection && message.starts_with(kPolynomialPrefix)) {
            polynomial_m_ = parse_leading_digits(message.substr(kPolynomialPrefix.size()));
            return;
        }
        if (entry.level != LogLevel::Info) {
            return;
        }
        // Both records name the untrimmed matrix. Their post-trim variants add
        // " post-trim" before the colon and are skipped.
        if (message.starts_with(kDegeneratePrefix)) {
            if (message.substr(kDegeneratePrefix.size()).starts_with(':')) {
                degenerate_rows_ = parse_count(message.substr(kDegeneratePrefix.size() + 1));
            }
            return;
        }
        if (message.starts_with(kFallbackPrefix)) {
            if (message.substr(kFallbackPrefix.size()).starts_with(':')) {
                fallbacks_ = parse_count(message.substr(kFallbackPrefix.size() + 1));
            }
        }
    }

    [[nodiscard]] RunEvidence snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return {polynomial_m_, degenerate_rows_, fallbacks_, fallback_warnings_};
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::string> polynomial_m_;
    std::optional<Count> degenerate_rows_;
    std::optional<Count> fallbacks_;
    size_t fallback_warnings_ = 0;
};

struct EnvironmentBinding final {
    const char* name;
    const char* value; // nullptr removes the variable for the run
};

using EnvironmentGuards = std::vector<std::unique_ptr<ScopedEnvironmentVariable>>;

// The isolation set of tests/test_gnfs_e2e_ladder.cpp on its default route:
// no resume, no distributed workers, and the experimental routes off.
std::vector<EnvironmentBinding> test_environment() {
    return {
        {"GNFS_DISABLE_SIQS", "1"},
        {"GNFS_STRUCTURED_FILTER", nullptr},
        {"GNFS_OOC_RELATIONS", nullptr},
        {"GNFS_OOC_BASE_PATH", nullptr},
        {"GNFS_RESUME", nullptr},
        {"GNFS_SIEVE_RESUME", nullptr},
        {"GNFS_DISTRIBUTED_SIEVE_WORKERS", "0"},
        {"GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "0"},
        {"GNFS_ADAPTIVE_LATTICE", "0"},
        {"GNFS_ADAPTIVE_LATTICE_THRESHOLD", nullptr},
        {"GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", nullptr},
        {"GNFS_ADAPTIVE_LATTICE_SEED", nullptr},
        {"GNFS_3LP", "0"},
        {"GNFS_CASCADE_V3", "0"},
        {"GNFS_V0_BFS", "0"},
        {"GNFS_OVERRIDE_LP_BITS", nullptr},
        {"GNFS_SIEVE_TARGET_MULT", nullptr},
        {"GNFS_LATTICE_LLL", nullptr},
        {"GNFS_LATTICE_SKEW", "0"},
        {"GNFS_SGE_STREAMING", "off"},
        {"GNFS_COFACTOR_BRENT", "0"},
        {"GNFS_ECM_BRENT_SUYAMA", "0"},
        {"GNFS_ECM_BS_DEGREE", nullptr},
        {"GNFS_ECM_STAGE1_PARALLEL_THREADS", "1"},
        {"GNFS_ECM_STAGE2_PARALLEL", "1"},
        {"GNFS_BRENT_POLLARD_RHO_THREADS", "1"},
        {"GNFS_ECM_CURVE_POOL", "0"},
        {"GNFS_COFACTOR_BATCH_SIZE", "1"},
        {"GNFS_ECM_SIGMA_POOL_SIZE", "0"},
        {"GNFS_ECM_B1_CACHE_SIZE", "0"},
        {"GNFS_COFACTOR_RESULT_CACHE_SIZE", "0"},
        {"GNFS_ECM_BATCH_INV", "0"},
        {"GNFS_SURVIVAL_FILTER", "0"},
        {"GNFS_SURVIVAL_THRESHOLD", nullptr},
        {"GNFS_COFACTOR_TIMING_ENABLE", "0"},
        {"GNFS_TRIAL_DIV_SIMD", "auto"},
    };
}

// Applies the process variables and returns the guards that restore the
// previous values.
[[nodiscard]] EnvironmentGuards freeze_environment() {
    EnvironmentGuards guards;
    for (const EnvironmentBinding& binding : test_environment()) {
        guards.push_back(std::make_unique<ScopedEnvironmentVariable>(binding.name, binding.value));
    }
    return guards;
}

// Digits of n in base m, low degree first: the coefficients of the base-m
// polynomial of the given degree, as BaseMSelector builds them.
std::vector<Integer> base_m_coefficients(const Integer& n, const Integer& m, size_t degree) {
    std::vector<Integer> coefficients;
    Integer remainder = n;
    for (size_t i = 0; i < degree; ++i) {
        Integer digit;
        Integer::divmod(remainder, digit, remainder, m);
        coefficients.push_back(std::move(digit));
    }
    coefficients.push_back(std::move(remainder));
    return coefficients;
}

std::string coefficients_text(const std::vector<Integer>& coefficients) {
    std::string text = "[";
    for (size_t i = 0; i < coefficients.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += coefficients[i].to_string();
    }
    text += "]";
    return text;
}

// Runs the forced-GNFS factorization and fills the evidence of the run.
FactorResult run(RunEvidence& evidence) {
    const Integer n(std::string(kN));
    const EnvironmentGuards environment = freeze_environment();

    // Declared before the Pipeline so that the callback never outlives its
    // state.
    EvidenceRecorder recorder;
    Config config;
    config.set_method(FactorizationMethod::GNFS).set_verbose(false);
    Pipeline pipeline(n, config);
    pipeline.set_log_callback([&recorder](const LogEntry& entry) { recorder.observe(entry); });

    FactorResult result = pipeline.run();
    evidence = recorder.snapshot();
    return result;
}

void verify(const FactorResult& result, const RunEvidence& evidence) {
    const Integer n(std::string(kN));
    const Integer expected_p(std::string(kExpectedP));
    const Integer expected_q(std::string(kExpectedQ));
    const FactorStats& stats = result.stats;

    if (evidence.degenerate_rows.has_value()) {
        std::cout << "  schirokauer degenerate rows: " << evidence.degenerate_rows->value << "/"
                  << evidence.degenerate_rows->total << "\n";
    }
    if (evidence.precision_fallbacks.has_value()) {
        std::cout << "  schirokauer precision fallbacks: " << evidence.precision_fallbacks->value
                  << "/" << evidence.precision_fallbacks->total << "\n";
    }
    std::cout << "  schirokauer precision-fallback warnings: "
              << evidence.precision_fallback_warnings << "\n";

    // The GNFS pipeline produced the factors. Trial division and Pollard rho
    // run as probes, but SIQS must not start at all.
    GNFS_TEST_CHECK(result.success);
    GNFS_TEST_CHECK(result.factors.size() == 2);
    const Integer& factor1 = result.factors[0];
    const Integer& factor2 = result.factors[1];
    const bool ordered = factor1 == expected_p && factor2 == expected_q;
    const bool reversed = factor1 == expected_q && factor2 == expected_p;
    GNFS_TEST_CHECK(ordered || reversed);
    GNFS_TEST_CHECK(stats.method_used == FactorizationMethod::GNFS);
    GNFS_TEST_CHECK(stats.executed_algorithm == "gnfs");
    GNFS_TEST_CHECK(std::find(stats.algorithms_executed.begin(), stats.algorithms_executed.end(),
                              "siqs") == stats.algorithms_executed.end());
    GNFS_TEST_CHECK(stats.matrix_rows > 0);

    // Precondition: the selected polynomial has exactly one linear factor
    // modulo 2, so its Schirokauer map takes the path that used to fall back.
    // The selection is read from the "m=" record; the coefficients are the
    // digits of N in base m.
    GNFS_TEST_CHECK(stats.degree == 3);
    GNFS_TEST_CHECK(evidence.polynomial_m.has_value());
    const Integer m(*evidence.polynomial_m);
    const std::vector<Integer> coefficients = base_m_coefficients(n, m, 3);
    GNFS_TEST_CHECK(coefficients.size() == 4);
    GNFS_TEST_CHECK(coefficients[3].is_one());
    std::cout << "  polynomial: m=" << m.to_string() << ", f = " << coefficients_text(coefficients)
              << "\n";
    const PolynomialContext ctx(n, coefficients, m);
    GNFS_TEST_CHECK(ctx.verify());
    // A cubic over F_2 has a linear factor when 0 or 1 is a root: 0 is a root
    // when the constant term is even, and 1 is a root when the sum of the
    // coefficients is even.
    size_t odd_coefficients = 0;
    for (const Integer& coefficient : coefficients) {
        odd_coefficients += coefficient.is_odd() ? size_t{1} : size_t{0};
    }
    const size_t roots_mod_2 = (coefficients[0].is_even() ? size_t{1} : size_t{0}) +
                               (odd_coefficients % 2 == 0 ? size_t{1} : size_t{0});
    GNFS_TEST_CHECK(roots_mod_2 == 1);
    const SchirokaurMap map(ctx);
    GNFS_TEST_CHECK(map.prime_info_.size() == 1);
    const auto& info = map.prime_info_[0];
    GNFS_TEST_CHECK(info.is_split);
    GNFS_TEST_CHECK(info.factors.size() == 2);
    GNFS_TEST_CHECK(info.factors[0].degree == 1);
    GNFS_TEST_CHECK(info.factors[1].degree == 2);

    // No relation of the run lost a Schirokauer column to precision, and the
    // pipeline reported the count without a warning.
    GNFS_TEST_CHECK(evidence.precision_fallbacks.has_value());
    GNFS_TEST_CHECK(evidence.precision_fallbacks->value == 0);
    GNFS_TEST_CHECK(evidence.precision_fallback_warnings == 0);
    GNFS_TEST_CHECK(evidence.degenerate_rows.has_value());
}

} // namespace

int main() {
    const Integer n(std::string(kN));
    std::cout << "=== Schirokauer precision end-to-end (N = " << n.to_string() << ") ===\n";

    RunEvidence evidence;
    int exit_code = 1;
    const auto start = std::chrono::steady_clock::now();
    try {
        const FactorResult result = run(evidence);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        verify(result, evidence);
        std::cout << "  PASS (" << seconds << "s)\n";
        exit_code = 0;
    } catch (const std::exception& error) {
        std::cerr << "test_schirokauer_precision_e2e: " << error.what() << '\n';
    }
    return exit_code;
}
