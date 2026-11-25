// Standalone experiment comparing frozen legacy code with the public production norm.
// See docs/perf/algebraic-norm-horner.md for the measurement protocol.
// Build against the production header under evaluation; no local Horner prototype is used.
// Printed baseline means legacy_norm; printed candidate means context.algebraic_norm.
// Only valid nonempty contexts are compared. Native fallback fractions are synthetic probes.
#include "gnfs/core/polynomial_context.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <gmp.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define NORM_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define NORM_NOINLINE __attribute__((noinline))
#else
#define NORM_NOINLINE
#endif
#ifndef GNFS_NORM_BENCH_REVISION
#define GNFS_NORM_BENCH_REVISION "unspecified"
#endif

namespace {
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr std::size_t sample_count = 256;
constexpr std::uint64_t initial_digest = UINT64_C(0x6a09e667f3bcc909);

struct BudgetExceeded : std::exception {};
void check_deadline(Deadline deadline) {
    if (Clock::now() >= deadline) {
        throw BudgetExceeded{};
    }
}

std::uint64_t magnitude(std::int64_t value) {
    return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                     : static_cast<std::uint64_t>(value);
}

// Raw import is independent of Integer's scalar arithmetic and works on LLP64.
void import_u64(mpz_ptr destination, std::uint64_t value) {
    mpz_import(destination, 1, 1, sizeof(value), 0, 0, &value);
}
void import_i64(mpz_ptr destination, std::int64_t value) {
    import_u64(destination, magnitude(value));
    if (value < 0) {
        mpz_neg(destination, destination);
    }
}

// Independent oracle: separately compute each power and multiply each term.
// Do not use context.algebraic_norm(), context.evaluate(), or a Horner loop.
Integer direct_gmp_sum(const PolynomialContext& context, std::int64_t a, std::uint64_t b) {
    Integer a_value, b_value, a_power, b_power, term, result;
    import_i64(a_value.get_mpz(), a);
    import_u64(b_value.get_mpz(), b);
    for (std::uint32_t i = 0; i <= context.degree(); ++i) {
        mpz_pow_ui(a_power.get_mpz(), a_value.get_mpz(), static_cast<unsigned long>(i));
        mpz_pow_ui(b_power.get_mpz(), b_value.get_mpz(),
                   static_cast<unsigned long>(context.degree() - i));
        mpz_mul(term.get_mpz(), a_power.get_mpz(), b_power.get_mpz());
        mpz_mul(term.get_mpz(), term.get_mpz(), context.coeff(i).get_mpz());
        mpz_add(result.get_mpz(), result.get_mpz(), term.get_mpz());
    }
    return result;
}

// Frozen legacy implementation from commit ca8e3c56820c8e3cdba65a5fb3d84fe968394dbb.
// Source: include/gnfs/core/polynomial_context.hpp
// Full source header SHA-256: b8a7f08a01f34cc174da13816e99a15b6029a19c66ffce958eafe5f7b698f895
// Original method text SHA-256: a9258bc0afbbec330779ae476888d540e32ed83b91fe431674904b99bc0ee82c
// Private degree_/f_coeffs_ become public accessors; the body is otherwise unchanged.
// Only valid nonempty contexts are compared, as required by the legacy implementation.
inline Integer legacy_norm(const PolynomialContext& context, std::int64_t a, std::uint64_t b) {
    // thread_local workspace: hot path in sieve cofactor (~100K calls/sec).
    // MAX_DEG=8 covers all practical GNFS degrees.
    static constexpr uint32_t MAX_DEG = 8;
    thread_local Integer ws_a_power, ws_term;
    thread_local std::array<Integer, MAX_DEG + 1> ws_b_powers;
    thread_local std::vector<Integer> ws_b_powers_heap;

    Integer* b_powers;
    if (context.degree() <= MAX_DEG) {
        b_powers = ws_b_powers.data();
    } else {
        ws_b_powers_heap.resize(context.degree() + 1);
        b_powers = ws_b_powers_heap.data();
    }

    // GMP's *_ui APIs accept unsigned long, which is only 32 bits on
    // Windows LLP64.  Keep the full uint64_t operand for wide b values.
    const bool b_fits_ulong = b <= std::numeric_limits<unsigned long>::max();
    Integer b_value;
    if (!b_fits_ulong) {
        b_value = b;
    }

    Integer result; // return value — must be independent (thread_local can't escape)
    ws_a_power = int64_t(1);
    b_powers[0] = int64_t(1);
    for (uint32_t i = 1; i <= context.degree(); ++i) {
        if (b_fits_ulong) {
            mpz_mul_ui(b_powers[i].get_mpz(), b_powers[i - 1].get_mpz(),
                       static_cast<unsigned long>(b));
        } else {
            mpz_mul(b_powers[i].get_mpz(), b_powers[i - 1].get_mpz(), b_value.get_mpz());
        }
    }

    // term = a^i * b^{d-i}, then result += f_i * term via mpz_addmul (fused FMA)
    // Note: No sign alternation for N(a - bα) = b^d * f(a/b)
    for (uint32_t i = 0; i <= context.degree(); ++i) {
        mpz_mul(ws_term.get_mpz(), ws_a_power.get_mpz(), b_powers[context.degree() - i].get_mpz());
        mpz_addmul(result.get_mpz(), context.coefficients()[i].get_mpz(), ws_term.get_mpz());

        if (i < context.degree()) {
            ws_a_power *= a;
        }
    }

    return result;
}

NORM_NOINLINE Integer baseline_norm(const PolynomialContext& context, std::int64_t a,
                                    std::uint64_t b) {
    return legacy_norm(context, a, b);
}

// Keep the same noinline timing boundary, but call the actual production method.
NORM_NOINLINE Integer candidate_norm(const PolynomialContext& context, std::int64_t a,
                                     std::uint64_t b) {
    return context.algebraic_norm(a, b);
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    return (hash ^ value) * UINT64_C(1099511628211);
}

// Every output limb and its sign affect the printed digest. This common cost
// is included in both timings; ratios describe the complete measured operation.
std::uint64_t digest_result(std::uint64_t hash, const Integer& result) {
    hash = mix(hash, static_cast<std::uint64_t>(mpz_sgn(result.get_mpz()) + 1));
    const std::size_t limbs = mpz_size(result.get_mpz());
    hash = mix(hash, static_cast<std::uint64_t>(limbs));
    for (std::size_t i = 0; i < limbs; ++i) {
        hash = mix(hash, static_cast<std::uint64_t>(
                             mpz_getlimbn(result.get_mpz(), static_cast<mp_size_t>(i))));
    }
    return hash;
}

struct Point {
    std::int64_t a;
    std::uint64_t b;
};

std::uint64_t next_random(std::uint64_t& state) {
    state += UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

std::vector<Point> make_points(unsigned a_bits, unsigned b_bits, std::uint64_t seed) {
    std::vector<Point> result;
    result.reserve(sample_count);
    const std::uint64_t a_mask = (std::uint64_t{1} << a_bits) - 1;
    const std::uint64_t b_mask = (std::uint64_t{1} << b_bits) - 1;
    while (result.size() < sample_count) {
        const std::uint64_t a_magnitude = next_random(seed) & a_mask;
        const std::uint64_t b = (next_random(seed) & b_mask) + 1;
        if (std::gcd(a_magnitude, b) != 1) {
            continue;
        }
        const auto signed_a = static_cast<std::int64_t>(a_magnitude);
        result.push_back({(result.size() & 1U) == 0 ? signed_a : -signed_a, b});
    }
    return result;
}

std::vector<Point> boundary_points() {
    const std::int64_t values_a[] = {(std::numeric_limits<std::int64_t>::min)(),
                                     (std::numeric_limits<std::int64_t>::min)() + 1,
                                     -((std::int64_t{1} << 34) + 7),
                                     -7,
                                     -1,
                                     0,
                                     1,
                                     7,
                                     (std::int64_t{1} << 34) + 7,
                                     (std::numeric_limits<std::int64_t>::max)()};
    const std::uint64_t values_b[] = {0,
                                      1,
                                      2,
                                      3,
                                      UINT32_MAX,
                                      std::uint64_t{1} << 32,
                                      (std::uint64_t{1} << 34) + 3,
                                      std::uint64_t{1} << 63,
                                      UINT64_MAX};
    std::vector<Point> result;
    for (const auto a : values_a) {
        for (const auto b : values_b) {
            result.push_back({a, b});
        }
    }
    return result;
}

Integer direct_polynomial_sum(const std::vector<Integer>& coefficients, const Integer& m) {
    Integer result, power, term;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        mpz_pow_ui(power.get_mpz(), m.get_mpz(), static_cast<unsigned long>(i));
        mpz_mul(term.get_mpz(), coefficients[i].get_mpz(), power.get_mpz());
        mpz_add(result.get_mpz(), result.get_mpz(), term.get_mpz());
    }
    return result;
}

PolynomialContext balanced_base_m(const char* n_decimal, unsigned degree) {
    Integer n(n_decimal), m, remaining(n), remainder, quotient, half;
    mpz_root(m.get_mpz(), n.get_mpz(), degree);
    if (mpz_cmp_ui(m.get_mpz(), 2) < 0) {
        throw std::runtime_error("base-m root is too small");
    }
    mpz_fdiv_q_2exp(half.get_mpz(), m.get_mpz(), 1);
    std::vector<Integer> coefficients;
    while (mpz_sgn(remaining.get_mpz()) != 0) {
        mpz_fdiv_qr(quotient.get_mpz(), remainder.get_mpz(), remaining.get_mpz(), m.get_mpz());
        if (mpz_cmp(remainder.get_mpz(), half.get_mpz()) > 0) {
            mpz_sub(remainder.get_mpz(), remainder.get_mpz(), m.get_mpz());
            mpz_add_ui(quotient.get_mpz(), quotient.get_mpz(), 1);
        }
        coefficients.emplace_back(remainder);
        mpz_set(remaining.get_mpz(), quotient.get_mpz());
    }
    const auto reconstructed = direct_polynomial_sum(coefficients, m);
    if (mpz_cmp(reconstructed.get_mpz(), n.get_mpz()) != 0) {
        throw std::runtime_error("independent base-m identity f(m) = N failed");
    }
    PolynomialContext context(std::move(n), std::move(coefficients), std::move(m));
    if (context.degree() != degree) {
        throw std::runtime_error("base-m degree differs from requested band");
    }
    return context;
}

struct Band {
    const char* name;
    const char* source;
    PolynomialContext context;
    unsigned a_bits;
    unsigned b_bits;
};

std::vector<Band> make_bands() {
    std::vector<Band> bands;
    bands.push_back({"gate81", "tests/test_regression_gate.cpp:LEVELS[3]",
                     balanced_base_m("1669994516749619561652133", 3), 20, 16});
    // Product independently checked in main against the two repository literals.
    bands.push_back({"kleinjung130", "tests/test_kleinjung_large.cpp:40-digit_case",
                     balanced_base_m("1219326311370217952447340343332251181129", 4), 24, 20});
    bands.push_back({"stress50d", "tests/test_stress.cpp:get_test_cases_level_1",
                     balanced_base_m("16000000000000004000000216000000000000027000000729", 4), 28,
                     24});
    bands.push_back(
        {"stress60d", "tests/test_stress.cpp:get_test_cases_level_2",
         balanced_base_m("160000000000000000000000040075200000000000000000000006908211", 5), 28,
         24});
    return bands;
}

void verify_number_product(const Integer& n, const char* left, const char* right) {
    Integer lhs(left), rhs(right), product;
    mpz_mul(product.get_mpz(), lhs.get_mpz(), rhs.get_mpz());
    if (mpz_cmp(product.get_mpz(), n.get_mpz()) != 0) {
        throw std::runtime_error("repository factor literals do not reconstruct N");
    }
}

std::uint64_t check_points(const PolynomialContext& context, const std::vector<Point>& points,
                           Deadline deadline) {
    std::uint64_t digest = initial_digest;
    for (const auto& point : points) {
        check_deadline(deadline);
        const Integer expected = direct_gmp_sum(context, point.a, point.b);
        const Integer baseline = baseline_norm(context, point.a, point.b);
        const Integer candidate = candidate_norm(context, point.a, point.b);
        if (mpz_cmp(expected.get_mpz(), baseline.get_mpz()) != 0 ||
            mpz_cmp(expected.get_mpz(), candidate.get_mpz()) != 0) {
            throw std::runtime_error(
                "norm mismatch: a=" + std::to_string(point.a) + " b=" + std::to_string(point.b) +
                " expected=" + expected.to_string() + " baseline=" + baseline.to_string() +
                " candidate=" + candidate.to_string());
        }
        digest = digest_result(digest, expected);
    }
    return digest;
}

void check_synthetic_boundaries(const std::vector<Point>& points) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    // Nonempty coefficient arrays only. Default/empty contexts belong to PR #355.
    const std::vector<std::vector<const char*>> fixtures{
        {"0"},
        {"-7"},
        {"-11", "3"},
        {"-6", "1", "-3", "2"},
        {"-123456789012345678901234567890", "0", "-9223372036854775808", "7", "-3", "0", "1", "-11",
         "2"},
        {"-9", "5", "-1", "7", "-3", "4", "0", "-2", "1", "3"},
        {"-1", "7", "-3", "0", "0"}};
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        std::vector<Integer> coefficients;
        for (const auto* value : fixtures[index]) {
            coefficients.emplace_back(value);
        }
        Integer m(17);
        Integer n = direct_polynomial_sum(coefficients, m);
        mpz_abs(n.get_mpz(), n.get_mpz());
        if (mpz_sgn(n.get_mpz()) == 0) {
            mpz_set_ui(n.get_mpz(), 1);
        }
        PolynomialContext context(std::move(n), std::move(coefficients), std::move(m));
        const auto digest = check_points(context, points, deadline);
        std::cout << "correctness fixture=" << index << " degree=" << context.degree()
                  << " points=" << points.size() << " digest=" << digest << '\n';
    }
}

using NormFunction = Integer (*)(const PolynomialContext&, std::int64_t, std::uint64_t);
struct Timing {
    double milliseconds;
    std::uint64_t digest;
};

Timing measure(NormFunction operation, const PolynomialContext& context,
               const std::vector<Point>& points, std::size_t calls, Deadline deadline) {
    std::uint64_t digest = initial_digest;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < calls; ++i) {
        if ((i & 255U) == 0) {
            check_deadline(deadline);
        }
        const auto& point = points[i % points.size()];
        const Integer result = operation(context, point.a, point.b);
        digest = digest_result(digest, result);
    }
    const auto stop = Clock::now();
    if (stop >= deadline) {
        throw BudgetExceeded{};
    }
    return {std::chrono::duration<double, std::milli>(stop - start).count(), digest};
}

struct Options {
    std::size_t rounds = 7;
    std::size_t calls = 32'768;
    std::size_t band_ms = 5'000;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            throw std::runtime_error("expected value after option");
        }
        const std::string_view flag(argv[i]), value(argv[i + 1]);
        std::size_t parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size()) {
            throw std::runtime_error("invalid unsigned option value");
        }
        if (flag == "--rounds")
            options.rounds = parsed;
        else if (flag == "--calls")
            options.calls = parsed;
        else if (flag == "--band-ms")
            options.band_ms = parsed;
        else
            throw std::runtime_error("unknown option");
    }
    if (options.rounds < 3 || options.rounds > 31 || options.calls < sample_count ||
        options.calls > 1'048'576 || options.calls % sample_count != 0 || options.band_ms < 100 ||
        options.band_ms > 5'000) {
        throw std::runtime_error("rounds:3..31; calls:256..1048576 and divisible by 256; "
                                 "band-ms:100..5000");
    }
    return options;
}

void print_input(const Band& band, const std::vector<Point>& points, std::uint64_t seed) {
    unsigned a_min = 64, a_max = 0, b_min = 64, b_max = 0;
    std::size_t coefficient_bits = 0, norm_bits = 0;
    std::size_t native_i128_hits = 0;
#if defined(__SIZEOF_INT128__)
    constexpr bool native_i128_supported = true;
#else
    constexpr bool native_i128_supported = false;
#endif
    std::uint64_t digest = initial_digest;
    for (const auto& point : points) {
        const auto a_width = static_cast<unsigned>(std::bit_width(magnitude(point.a)));
        const auto b_width = static_cast<unsigned>(std::bit_width(point.b));
        a_min = std::min(a_min, a_width);
        a_max = std::max(a_max, a_width);
        b_min = std::min(b_min, b_width);
        b_max = std::max(b_max, b_width);
        digest = mix(mix(digest, static_cast<std::uint64_t>(point.a)), point.b);
        const auto norm = direct_gmp_sum(band.context, point.a, point.b);
        norm_bits = std::max(norm_bits, norm.bit_length());
#if defined(__SIZEOF_INT128__)
        // Untimed dispatch probe only. Production first tries this native
        // path, so a faster GMP helper cannot benefit native-hit samples.
        if (band.context.algebraic_norm_i128(point.a, point.b).second) {
            ++native_i128_hits;
        }
#endif
    }
    for (const auto& coefficient : band.context.coefficients()) {
        coefficient_bits = std::max(coefficient_bits, coefficient.bit_length());
    }
    std::cout << "input band=" << band.name << " source=" << band.source
              << " n=" << band.context.n().to_string()
              << " n_digits=" << band.context.n().to_string().size()
              << " n_bits=" << band.context.n().bit_length() << " degree=" << band.context.degree()
              << " m=" << band.context.m().to_string() << " coeff_max_bits=" << coefficient_bits
              << " norm_max_bits=" << norm_bits << " a_bits=" << a_min << ':' << a_max
              << " b_bits=" << b_min << ':' << b_max
              << " coordinate_source=synthetic_splitmix64_coprime"
              << " abs_a_limit_exclusive=" << (std::uint64_t{1} << band.a_bits)
              << " b_limit_inclusive=" << (std::uint64_t{1} << band.b_bits)
              << " samples=" << points.size() << " seed=" << seed
              << " native_i128_supported=" << native_i128_supported
              << " native_i128_probe_hits=" << native_i128_hits
              << " native_i128_probe_fallbacks=" << points.size() - native_i128_hits
              << " native_i128_probe_hit_fraction="
              << static_cast<double>(native_i128_hits) / static_cast<double>(points.size())
              << " native_i128_probe_fallback_fraction="
              << static_cast<double>(points.size() - native_i128_hits) /
                     static_cast<double>(points.size())
              << " input_digest=" << digest << " coefficients=";
    for (std::size_t i = 0; i < band.context.coefficients().size(); ++i) {
        if (i != 0)
            std::cout << ',';
        std::cout << band.context.coefficients()[i].to_string();
    }
    std::cout << '\n';
}

bool run_band(const Band& band, std::size_t index, const Options& options,
              const std::vector<Point>& boundaries) {
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(options.band_ms);
    const std::uint64_t seed = UINT64_C(0x2026111000000000) + index;
    const auto points = make_points(band.a_bits, band.b_bits, seed);
    print_input(band, points, seed);
    std::size_t completed_pairs = 0;
    std::vector<double> ratios;
    try {
        // Untimed warmups give both TLS paths one full traversal before measurement.
        (void)measure(baseline_norm, band.context, points, sample_count, deadline);
        (void)measure(candidate_norm, band.context, points, sample_count, deadline);
        for (std::size_t round = 0; round < options.rounds; ++round) {
            // Each measured pair gets a fresh complete independent oracle precheck.
            const auto oracle_digest = check_points(band.context, points, deadline);
            const auto boundary_digest = check_points(band.context, boundaries, deadline);
            Timing baseline{}, candidate{};
            const bool baseline_first = (round & 1U) == 0;
            if (baseline_first) {
                baseline = measure(baseline_norm, band.context, points, options.calls, deadline);
                candidate = measure(candidate_norm, band.context, points, options.calls, deadline);
            } else {
                candidate = measure(candidate_norm, band.context, points, options.calls, deadline);
                baseline = measure(baseline_norm, band.context, points, options.calls, deadline);
            }
            if (baseline.digest != candidate.digest) {
                throw std::runtime_error("measured output digests differ");
            }
            ++completed_pairs;
            const double ratio = baseline.milliseconds / candidate.milliseconds;
            ratios.push_back(ratio);
            std::cout << "round band=" << band.name << " index=" << round + 1
                      << " order=" << (baseline_first ? "baseline,candidate" : "candidate,baseline")
                      << " calls_each=" << options.calls << " baseline_ms=" << baseline.milliseconds
                      << " candidate_ms=" << candidate.milliseconds
                      << " baseline_over_candidate=" << ratio
                      << " baseline_digest=" << baseline.digest
                      << " candidate_digest=" << candidate.digest
                      << " oracle_digest=" << oracle_digest
                      << " boundary_digest=" << boundary_digest << '\n';
        }
    } catch (const BudgetExceeded&) {
        std::cout << "band_status band=" << band.name
                  << " status=budget_exhausted completed_pairs=" << completed_pairs
                  << " requested_pairs=" << options.rounds << " elapsed_ms="
                  << std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                  << '\n';
        return false;
    }
    std::sort(ratios.begin(), ratios.end());
    const auto middle = ratios.size() / 2;
    const double median =
        ratios.size() % 2 == 0 ? (ratios[middle - 1] + ratios[middle]) / 2 : ratios[middle];
    std::cout << "band_status band=" << band.name
              << " status=complete completed_pairs=" << completed_pairs
              << " median_baseline_over_candidate=" << median << " min_ratio=" << ratios.front()
              << " max_ratio=" << ratios.back() << " elapsed_ms="
              << std::chrono::duration<double, std::milli>(Clock::now() - start).count() << '\n';
    return true;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "experiment=algebraic_norm_microbenchmark revision="
                  << GNFS_NORM_BENCH_REVISION << " gmp=" << gmp_version
                  << " sizeof_long=" << sizeof(long) << " sizeof_pointer=" << sizeof(void*)
                  << " ndebug="
#ifdef NDEBUG
                  << 1
#else
                  << 0
#endif
                  << " rounds=" << options.rounds << " calls=" << options.calls
                  << " band_budget_ms=" << options.band_ms
                  << " coordinates=synthetic_coprime pipeline_benefit=unmeasured\n";
        const auto boundaries = boundary_points();
        check_synthetic_boundaries(boundaries);
        auto bands = make_bands();
        verify_number_product(bands[0].context.n(), "1292282676071", "1292282677523");
        verify_number_product(bands[1].context.n(), "12345678901234567891", "98765432109876543219");
        verify_number_product(bands[2].context.n(), "4000000000000000000000027",
                              "4000000000000001000000027");
        verify_number_product(bands[3].context.n(), "400000000000000000000000000069",
                              "400000000000000000000000100119");
        bool complete = true;
        for (std::size_t index = 0; index < bands.size(); ++index) {
            complete = run_band(bands[index], index, options, boundaries) && complete;
        }
        std::cout << "experiment_status=" << (complete ? "complete" : "incomplete")
                  << " scope=norm_microbenchmark pipeline_benefit=unmeasured\n";
        return complete ? 0 : 2;
    } catch (const BudgetExceeded&) {
        std::cerr << "correctness_status=budget_exhausted\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "experiment_status=failed reason=" << error.what() << '\n';
        return 1;
    }
}
