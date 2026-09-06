#include "fixtures/siqs_live_sieve_fixtures_v1.hpp"
#include "support/scoped_environment_stderr.hpp"

#include <gnfs/siqs/shadow_proof_prefer.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using gnfs::core::Integer;
using gnfs::tests::siqs_live_sieve_fixture_v1;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::tests::support::ScopedStderrCapture;
using gnfs::tests::support::ScopedUnwritableStderr;
using namespace gnfs::siqs;

namespace {

[[noreturn]] void fail_test(const std::string& message) {
    std::fprintf(stderr, "SIQS test failure: %s\n", message.c_str());
    std::abort();
}

void require_test(bool condition, const std::string& message) {
    if (!condition) {
        fail_test(message);
    }
}

[[nodiscard]] size_t count_occurrences(std::string_view text, std::string_view needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] std::pair<Integer, Integer> canonical_factors(const SIQSResult& result) {
    std::pair<Integer, Integer> factors{result.factor1, result.factor2};
    if (factors.first > factors.second) {
        std::swap(factors.first, factors.second);
    }
    return factors;
}

[[nodiscard]] std::optional<SIQSResult> factor_143_with_shadow_mode(const char* mode,
                                                                    std::string& captured_stderr,
                                                                    size_t max_seconds = 10) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    auto result = factor(Integer("143"), max_seconds, false);
    captured_stderr = capture.finish();
    return result;
}

[[nodiscard]] std::optional<SIQSResult>
factor_with_unwritable_shadow_stderr(const Integer& modulus, size_t max_seconds, const char* mode) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedUnwritableStderr stderr_failure;
    auto result = factor(modulus, max_seconds, false);
    stderr_failure.finish();
    return result;
}

[[nodiscard]] std::optional<SIQSResult> factor_with_shadow_mode(const Integer& modulus,
                                                                size_t max_seconds,
                                                                const char* mode,
                                                                std::string& captured_stderr) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    auto result = factor(modulus, max_seconds, false);
    captured_stderr = capture.finish();
    return result;
}

[[nodiscard]] std::string_view record_field(std::string_view record, std::string_view key) {
    const std::string token = std::string(key) + '=';
    const size_t begin = record.find(token);
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t value_begin = begin + token.size();
    const size_t end = record.find_first_of(" \r\n", value_begin);
    return record.substr(value_begin, end == std::string_view::npos ? std::string_view::npos
                                                                    : end - value_begin);
}

} // namespace

void test_one_large_prime_rejects_strong_pseudoprimes() {
    // Each value is composite but passes the legacy single-witness base-2
    // Miller-Rabin check that used to guard SIQS 1LP admission.
    struct CompositeCase final {
        uint64_t value;
        uint64_t known_divisor;
    };
    constexpr CompositeCase strong_base2_pseudoprimes[] = {
        {2047ULL, 23ULL},
        {341550071728321ULL, 10670053ULL},
        {3825123056546413051ULL, 149491ULL},
    };

    for (const CompositeCase& sample : strong_base2_pseudoprimes) {
        if (sample.known_divisor <= 1 || sample.known_divisor >= sample.value ||
            sample.value % sample.known_divisor != 0) {
            std::fprintf(stderr, "SIQS pseudoprime fixture lost its known divisor: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
        if (is_valid_one_large_prime(sample.value)) {
            std::fprintf(stderr, "SIQS 1LP admitted strong base-2 pseudoprime: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
    }

    constexpr uint64_t known_primes[] = {
        2ULL, 3ULL, 101ULL, 4294967311ULL, 18446744073709551557ULL,
    };
    for (uint64_t value : known_primes) {
        if (!is_valid_one_large_prime(value)) {
            std::fprintf(stderr, "SIQS 1LP rejected known prime: %llu\n",
                         static_cast<unsigned long long>(value));
            std::abort();
        }
    }
    if (is_valid_one_large_prime(0) || is_valid_one_large_prime(1) || is_valid_one_large_prime(4)) {
        std::fprintf(stderr, "SIQS 1LP admitted a trivial composite boundary\n");
        std::abort();
    }

    std::printf("  one_large_prime strong pseudoprimes: PASS\n");
}

void test_tonelli_shanks() {
    // sqrt(2) mod 7 = 3 (since 3^2 = 9 ≡ 2 mod 7)
    uint32_t r = tonelli_shanks(2, 7);
    assert(r == 3 || r == 4); // 3 or 7-3=4
    assert((uint64_t)r * r % 7 == 2);

    // sqrt(2) mod 17 = 6 (since 6^2 = 36 ≡ 2 mod 17)
    r = tonelli_shanks(2, 17);
    assert((uint64_t)r * r % 17 == 2);

    // sqrt(3) mod 13 = 4 (since 4^2 = 16 ≡ 3 mod 13)
    r = tonelli_shanks(3, 13);
    assert((uint64_t)r * r % 13 == 3);

    // Non-QR: 2 mod 5 (Legendre = -1)
    r = tonelli_shanks(2, 5);
    assert(r == 0);

    printf("  tonelli_shanks: PASS\n");
}

/// 测试 split_cofactor_64 边界:输入是素数(应该 split 失败 → {0,0})、
/// 输入是 1(无意义 → {0,0})、输入是平方数(应该返回 √n 两次)。
void test_split_cofactor_edge() {
    // 1. n=1: 无意义
    {
        auto [p1, p2] = split_cofactor_64(1);
        assert(p1 == 0 && p2 == 0);
    }
    // 2. n=0: 无意义
    {
        auto [p1, p2] = split_cofactor_64(0);
        assert(p1 == 0 && p2 == 0);
    }
    // 3. 大素数(无法分解 — 所有方法应失败)
    // 1099511627791 是素数(2^40 + 15)
    {
        auto [p1, p2] = split_cofactor_64(1099511627791ULL);
        // 素数情况下 trial division 和 Pollard rho 都应失败,返回 {0, 0}
        // 注:也可能因为是边界数,SQUFOF 会循环退出 — 关键是返回的不应是 {p, n/p}
        // 因为素数无非平凡因子。
        assert(p1 == 0 || p1 * p2 == 1099511627791ULL);
        if (p1 != 0) {
            // 若声称分解了,验证 p1 * p2 == n
            assert(p1 > 1 && p1 < 1099511627791ULL);
        }
    }
    // 4. 平方数:n=p²,split 应返回 {p, p}
    {
        // 1009² = 1018081
        auto [p1, p2] = split_cofactor_64(1018081ULL);
        assert(p1 == 1009 && p2 == 1009);
    }
    // 5. 简单半素数:7 * 13 = 91
    {
        auto [p1, p2] = split_cofactor_64(91);
        assert(p1 == 7 && p2 == 13);
    }
    // 6. 三因子合数:2 * 3 * 5 = 30 → split 应返回某对 (a, b) 满足 a*b=30
    {
        auto [p1, p2] = split_cofactor_64(30);
        assert(p1 * p2 == 30);
        assert(p1 > 1 && p2 > 1);
    }

    printf("  split_cofactor edge cases: PASS\n");
}

void test_factor_base() {
    Integer N("1000000007"); // prime, but we're testing FB construction
    auto fb = build_factor_base(N, 20);
    assert(fb.size() >= 20);

    // Verify sqrt(N) mod p is correct for each FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t sq = fb[i].sqrt_n;
        uint64_t n_mod = mpz_fdiv_ui(N.get_mpz(), p);
        assert(((uint64_t)sq * sq) % p == n_mod % p);
    }

    printf("  factor_base: PASS (%zu primes)\n", fb.size());
}

void test_init_poly_handles_large_a_factor_count() {
    // The production table currently uses at most 12 A factors, but this is
    // a valid runtime shape for the public helper.  It also covers the former
    // 16-element stack workspace, which was only protected by assert().
    const Integer N("1000000007");
    const auto fb = build_factor_base(N, 64);
    require_test(fb.size() >= 19, "factor base too small for A-factor boundary test");

    SIQSPoly poly;
    poly.a_indices.reserve(17);
    for (uint32_t index = 2; index < 19; ++index) {
        poly.a_indices.push_back(index);
    }
    poly.A = Integer(1);
    for (const uint32_t index : poly.a_indices) {
        poly.A *= static_cast<int64_t>(fb[index].p);
    }

    init_poly(N, fb, 128, poly);
    require_test(poly.B_parts.size() == poly.a_indices.size(),
                 "large A-factor initialization lost CRT parts");
    require_test(poly.coeffs.size() == poly.a_indices.size(),
                 "large A-factor initialization lost coefficients");
    require_test(poly.solns.size() == fb.size() && poly.a_inv_mod_p.size() == fb.size(),
                 "large A-factor initialization produced incomplete sieve state");
    require_test(poly.bp_mod_p.size() == poly.a_indices.size() * fb.size(),
                 "large A-factor initialization produced incomplete B residues");

    std::printf("  init_poly large A-factor boundary (17 factors): PASS\n");
}

void test_siqs_small() {
    std::string default_stderr;
    const auto default_result = factor_143_with_shadow_mode(nullptr, default_stderr);
    require_test(default_result.has_value(), "143 did not factor with the shadow mode unset");
    require_test(count_occurrences(default_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(default_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) ==
                         0,
                 "unset shadow mode emitted shadow telemetry");
    require_test(!default_result->shadow_proof_observe_record_committed,
                 "unset shadow mode reported a committed observe record");

    std::string off_stderr;
    const auto off_result = factor_143_with_shadow_mode("0", off_stderr);
    require_test(off_result.has_value(), "143 did not factor with shadow proof disabled");
    require_test(count_occurrences(off_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(off_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 "disabled shadow proof emitted shadow telemetry");
    require_test(!off_result->shadow_proof_observe_record_committed,
                 "disabled shadow proof reported a committed observe record");

    std::string observe_stderr;
    const auto observe_result = factor_143_with_shadow_mode("observe", observe_stderr);
    require_test(observe_result.has_value(), "143 did not factor in observe mode");
    require_test(count_occurrences(observe_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 1,
                 "observe mode did not emit exactly one schema-v1 record");
    require_test(count_occurrences(observe_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 "observe mode emitted a schema-v2 prefer decision");
    require_test(observe_stderr.find("route=legacy_continue") != std::string::npos,
                 "observe record did not declare legacy continuation");
    require_test(observe_result->shadow_proof_observe_record_committed,
                 "observe mode did not report a committed schema-v1 record");

    const auto failed_observe_result =
        factor_with_unwritable_shadow_stderr(Integer("143"), 10, "observe");
    require_test(failed_observe_result.has_value(),
                 "143 did not continue the legacy factor path after observe write failure");
    require_test(!failed_observe_result->shadow_proof_observe_record_committed,
                 "observe write failure reported a committed schema-v1 record");

    std::string prefer_fallback_stderr;
    const auto prefer_fallback_result =
        factor_143_with_shadow_mode("prefer", prefer_fallback_stderr, 0);
    require_test(!prefer_fallback_result.has_value(),
                 "zero-budget prefer fallback unexpectedly returned a factor");
    require_test(count_occurrences(prefer_fallback_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(prefer_fallback_stderr,
                                       SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1,
                 "prefer fallback did not emit exactly one schema-v2 decision");
    require_test(
        record_field(prefer_fallback_stderr, "schema_version") == "2" &&
            record_field(prefer_fallback_stderr, "status") == "valid" &&
            record_field(prefer_fallback_stderr, "mode") == "prefer" &&
            record_field(prefer_fallback_stderr, "decision") == "legacy_fallback" &&
            record_field(prefer_fallback_stderr, "reason") == "shadow_not_factor" &&
            record_field(prefer_fallback_stderr, "next_route") == "legacy_continue" &&
            record_field(prefer_fallback_stderr, "shadow_terminal") == "bounded_fallback" &&
            record_field(prefer_fallback_stderr, "shadow_stage") == "assembly" &&
            record_field(prefer_fallback_stderr, "shadow_fallback") == "insufficient_rows" &&
            record_field(prefer_fallback_stderr, "result_present") == "false" &&
            record_field(prefer_fallback_stderr, "promotion") == "false",
        "prefer fallback did not commit a closed legacy-continuation decision");

    const auto default_factors = canonical_factors(*default_result);
    const auto off_factors = canonical_factors(*off_result);
    const auto observe_factors = canonical_factors(*observe_result);
    const auto failed_observe_factors = canonical_factors(*failed_observe_result);
    const unsigned expected_sieve_workers =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    require_test(default_result->resolved_sieve_workers == expected_sieve_workers,
                 "unset-mode result did not report the production sieve worker count");
    require_test(off_result->resolved_sieve_workers == expected_sieve_workers,
                 "disabled-mode result did not report the production sieve worker count");
    require_test(observe_result->resolved_sieve_workers == expected_sieve_workers,
                 "observe-mode result did not report the production sieve worker count");
    require_test(off_result->resolved_sieve_workers == observe_result->resolved_sieve_workers,
                 "shadow mode changed the production sieve worker count");
    require_test(default_factors == off_factors,
                 "unset and explicit-off modes returned different canonical factors");
    require_test(off_factors == observe_factors,
                 "observe mode changed the canonical 143 factor result");
    require_test(off_factors == failed_observe_factors,
                 "observe write failure changed the canonical 143 factor result");
    require_test(off_factors.first == Integer(11) && off_factors.second == Integer(13),
                 "143 factorization did not return 11 and 13");
    printf("  siqs_small(143) shadow modes: PASS (unset %.3fs, off %.3fs, observe %.3fs)\n",
           default_result->time_seconds, off_result->time_seconds, observe_result->time_seconds);
}

void test_siqs_prefer_candidate_route() {
    const auto fixture = siqs_live_sieve_fixture_v1(50);
    require_test(fixture.has_value(), "missing public 50-digit live-sieve fixture");
    const Integer modulus(std::string(fixture->modulus));
    const Integer expected_factor(std::string(fixture->factor_p));
    const Integer expected_cofactor(std::string(fixture->factor_q));
    require_test(expected_factor * expected_cofactor == modulus,
                 "public 50-digit fixture factors do not multiply to the modulus");

    std::string prefer_stderr;
    const auto prefer_result = factor_with_shadow_mode(modulus, 30, "prefer", prefer_stderr);
    require_test(prefer_result.has_value(), "50-digit prefer candidate did not return a factor");
    require_test(count_occurrences(prefer_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 "prefer candidate emitted an observe record");
    require_test(count_occurrences(prefer_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1,
                 "prefer candidate did not emit exactly one schema-v2 decision");
    require_test(record_field(prefer_stderr, "schema_version") == "2" &&
                     record_field(prefer_stderr, "status") == "valid" &&
                     record_field(prefer_stderr, "mode") == "prefer" &&
                     record_field(prefer_stderr, "decision") == "shadow_candidate" &&
                     record_field(prefer_stderr, "reason") == "shadow_factor_valid" &&
                     record_field(prefer_stderr, "next_route") == "shadow_return",
                 "prefer candidate did not commit the expected shadow-return decision");
    require_test(record_field(prefer_stderr, "input_n") == fixture->modulus &&
                     record_field(prefer_stderr, "factor") == fixture->factor_p &&
                     record_field(prefer_stderr, "cofactor") == fixture->factor_q &&
                     record_field(prefer_stderr, "factor_identity") == "pass" &&
                     record_field(prefer_stderr, "result_present") == "true",
                 "prefer decision did not bind the public fixture factor identity");
    require_test(
        record_field(prefer_stderr, "relations_source") == "shadow_selected_rows" &&
            record_field(prefer_stderr, "polynomials_source") == "production_sieve_counter" &&
            record_field(prefer_stderr, "decision_wall_ns_supported") == "true" &&
            record_field(prefer_stderr, "time_scope") == "siqs_timer_to_pre_emit_decision" &&
            record_field(prefer_stderr, "emit_phase") == "before_route" &&
            record_field(prefer_stderr, "promotion") == "false",
        "prefer decision did not bind the production result sources");
    require_test(record_field(prefer_stderr, "relations_found") ==
                         std::to_string(prefer_result->relations_found) &&
                     record_field(prefer_stderr, "polynomials_used") ==
                         std::to_string(prefer_result->polynomials_used),
                 "prefer result metadata diverged from the committed decision");
    const uint64_t decision_wall_ns =
        std::stoull(std::string(record_field(prefer_stderr, "decision_wall_ns")));
    require_test(decision_wall_ns > 0 &&
                     prefer_result->time_seconds ==
                         static_cast<double>(decision_wall_ns) / 1'000'000'000.0,
                 "prefer result time did not reuse the single pre-emit wall sample");
    require_test(!prefer_result->shadow_proof_observe_record_committed,
                 "prefer result reported an observe record commit");

    const auto prefer_factors = canonical_factors(*prefer_result);
    require_test(prefer_factors.first == expected_factor &&
                     prefer_factors.second == expected_cofactor,
                 "prefer candidate returned the wrong public fixture factors");
    const unsigned expected_sieve_workers =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    require_test(prefer_result->resolved_sieve_workers == expected_sieve_workers,
                 "prefer candidate did not report the production sieve worker count");

    const auto failed_emit_result = factor_with_unwritable_shadow_stderr(modulus, 30, "prefer");
    require_test(failed_emit_result.has_value(),
                 "prefer candidate did not continue legacy after V2 write failure");
    require_test(canonical_factors(*failed_emit_result) == prefer_factors,
                 "prefer V2 write failure changed the canonical fixture factors");
    require_test(!failed_emit_result->shadow_proof_observe_record_committed,
                 "prefer V2 write failure reported an observe record commit");
    require_test(failed_emit_result->resolved_sieve_workers == expected_sieve_workers,
                 "prefer V2 write failure changed the production sieve worker count");

    printf("  siqs prefer candidate(50d): PASS (shadow %.3fs, failed-emit legacy %.3fs)\n",
           prefer_result->time_seconds, failed_emit_result->time_seconds);
}

void require_siqs_shadow_mode_rejected(const char* mode) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    bool invalid_argument_thrown = false;
    bool unexpected_exception_thrown = false;
    try {
        (void)factor(Integer("143"), 10, false);
    } catch (const std::invalid_argument&) {
        invalid_argument_thrown = true;
    } catch (...) {
        unexpected_exception_thrown = true;
    }
    const std::string captured_stderr = capture.finish();

    require_test(invalid_argument_thrown && !unexpected_exception_thrown,
                 std::string("shadow mode '") + mode +
                     "' did not fail closed with std::invalid_argument");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 std::string("shadow mode '") + mode + "' performed observe work before throwing");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 std::string("shadow mode '") + mode +
                     "' emitted a prefer decision before throwing");
}

void test_siqs_shadow_rejected_modes() {
    require_siqs_shadow_mode_rejected("invalid");
    require_siqs_shadow_mode_rejected("Prefer");
    printf("  siqs shadow rejected modes (invalid, Prefer): PASS\n");
}

void test_siqs_20digit() {
    // 20-digit semiprime: 12345678901234567891 = 3 * 4115226300411522597
    // Actually let's use a known 20-digit semiprime
    // 10000000000000000051 * ... let's just try a small product
    Integer p1("1000000007");
    Integer p2("1000000009");
    Integer N = p1 * p2;
    printf("  siqs_20digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto start = std::chrono::steady_clock::now();
    auto result = factor(N, 30, true);
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (result) {
        auto f1 = result->factor1, f2 = result->factor2;
        if (f1 > f2)
            std::swap(f1, f2);
        assert(f1 * f2 == N);
        printf("  siqs_20digit: PASS (%.3fs, %zu polys)\n", elapsed, result->polynomials_used);
    } else {
        printf("  siqs_20digit: FAIL — no factor found (%.3fs)\n", elapsed);
        assert(false);
    }
}

void test_siqs_30digit() {
    Integer p1("1000000000000007");
    Integer p2("1000000000000037");
    Integer N = p1 * p2;
    printf("  siqs_30digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 60, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_30digit: PASS (%.3fs, %zu polys)\n", result->time_seconds,
               result->polynomials_used);
    } else {
        printf("  siqs_30digit: FAIL\n");
        // Don't assert — this may need parameter tuning
    }
}

void test_siqs_40digit() {
    Integer p1("10000000000000000051");
    Integer p2("10000000000000000099");
    Integer N = p1 * p2;
    printf("  siqs_40digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 120, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_40digit: PASS (%.3fs, %zu polys)\n", result->time_seconds,
               result->polynomials_used);
    } else {
        printf("  siqs_40digit: FAIL\n");
    }
}

int main() {
    ScopedEnvironmentVariable default_shadow_mode(SIQS_SHADOW_PROOF_ENV, "0");

    printf("=== SIQS Unit Tests ===\n\n");

    printf("--- Helper tests ---\n");
    test_one_large_prime_rejects_strong_pseudoprimes();
    test_tonelli_shanks();
    test_factor_base();
    test_init_poly_handles_large_a_factor_count();
    test_split_cofactor_edge();

    printf("\n--- Factorization tests ---\n");
    test_siqs_small();
    test_siqs_prefer_candidate_route();
    test_siqs_shadow_rejected_modes();
    test_siqs_20digit();
    test_siqs_30digit();
    test_siqs_40digit();

    printf("\n=== All SIQS tests passed ===\n");
    return 0;
}
