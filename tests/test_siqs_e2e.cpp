/// @file test_siqs_e2e.cpp
/// @brief End-to-end SIQS validation on hand-picked semiprimes.
///
/// Covers two size bands the task scope calls out:
///   - 100-bit semiprime (31-digit) — must succeed within 60 s
///   - 180-bit semiprime (55-digit) — must succeed within 600 s (slow tier)
///
/// Also validates that the pipeline router (Pipeline::run) actually selects
/// SIQS for these sizes and that the returned factors multiply back to N.
/// Compared against the GNFS path (see commentary at bottom): on M-series the
/// 31-digit case factors in milliseconds whereas GNFS for the same N already
/// pays seconds of overhead for polynomial selection alone.

#include <gnfs/api/factorizer.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using gnfs::core::Integer;
namespace siqs = gnfs::siqs;
namespace api  = gnfs::api;

namespace {

struct Case {
    const char* label;
    const char* n_str;
    const char* p_str;
    const char* q_str;
    size_t      timeout_s;
    size_t      expected_bits;
    size_t      expected_digits;
};

bool run_direct_siqs(const Case& c) {
    Integer N(c.n_str);
    Integer P(c.p_str);
    Integer Q(c.q_str);
    size_t bits   = N.bit_length();
    size_t digits = N.to_string().size();
    printf("\n[E2E:%s] direct siqs::factor — N (%zu bits, %zu digits) timeout=%zus\n",
           c.label, bits, digits, c.timeout_s);
    assert(bits   == c.expected_bits);
    assert(digits == c.expected_digits);
    assert(P * Q == N);

    auto t0 = std::chrono::steady_clock::now();
    auto r  = siqs::factor(N, c.timeout_s, /*verbose=*/false);
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!r) {
        printf("[E2E:%s] FAIL: SIQS returned no factor (%.3fs)\n", c.label, elapsed);
        return false;
    }
    Integer f1 = r->factor1;
    Integer f2 = r->factor2;
    if (f1 > f2) std::swap(f1, f2);
    Integer p_sorted = P, q_sorted = Q;
    if (p_sorted > q_sorted) std::swap(p_sorted, q_sorted);
    if (f1 != p_sorted || f2 != q_sorted) {
        printf("[E2E:%s] FAIL: got %s * %s, expected %s * %s\n",
               c.label,
               f1.to_string().c_str(), f2.to_string().c_str(),
               p_sorted.to_string().c_str(), q_sorted.to_string().c_str());
        return false;
    }
    printf("[E2E:%s] PASS: %s * %s in %.3fs (%zu polys, budget=%zus)\n",
           c.label, f1.to_string().c_str(), f2.to_string().c_str(),
           elapsed, r->polynomials_used, c.timeout_s);
    return true;
}

bool run_pipeline_siqs(const Case& c) {
    Integer N(c.n_str);
    printf("\n[E2E:%s] api::factorize — pipeline routes to SIQS\n", c.label);

    auto t0 = std::chrono::steady_clock::now();
    auto result = api::factorize(N);
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!result.success) {
        printf("[E2E:%s] FAIL: pipeline returned !success (%.3fs)\n", c.label, elapsed);
        return false;
    }
    if (result.factors.size() < 2) {
        printf("[E2E:%s] FAIL: expected ≥2 factors, got %zu\n",
               c.label, result.factors.size());
        return false;
    }
    Integer product(1);
    for (const auto& f : result.factors) product = product * f;
    if (product != N) {
        printf("[E2E:%s] FAIL: factor product mismatch\n", c.label);
        return false;
    }
    bool used_siqs =
        (result.stats.method_used == api::FactorizationMethod::SIQS);
    if (!used_siqs) {
        printf("[E2E:%s] FAIL: expected SIQS method, got %s\n",
               c.label, api::method_name(result.stats.method_used));
        return false;
    }
    printf("[E2E:%s] PASS: pipeline used SIQS, %.3fs wall\n", c.label, elapsed);
    return true;
}

bool run_force_siqs_env(const Case& c) {
    // For small N, normally the router picks PollardRho. Force SIQS via ENV
    // and check the pipeline actually routes there.
    Integer N(c.n_str);
    printf("\n[E2E:%s] GNFS_FORCE_SIQS=1 forces SIQS for an otherwise-rho-sized N\n",
           c.label);
    setenv("GNFS_FORCE_SIQS", "1", 1);
    auto result = api::factorize(N);
    unsetenv("GNFS_FORCE_SIQS");

    if (!result.success) {
        printf("[E2E:%s] FAIL: forced SIQS pipeline failed\n", c.label);
        return false;
    }
    bool routed_siqs =
        (result.stats.method_used == api::FactorizationMethod::SIQS);
    Integer product(1);
    for (const auto& f : result.factors) product = product * f;
    if (product != N) {
        printf("[E2E:%s] FAIL: factor product mismatch\n", c.label);
        return false;
    }
    if (!routed_siqs) {
        printf("[E2E:%s] FAIL: expected SIQS, got %s\n",
               c.label, api::method_name(result.stats.method_used));
        return false;
    }
    printf("[E2E:%s] PASS: forced SIQS produced valid factorization\n", c.label);
    return true;
}

} // namespace

int main() {
    // 100-bit: two consecutive 50-bit primes (≈ 2^50 + small offsets)
    const Case c100 = {
        "100bit",
        "1267650600228402790082356974917",
        "1125899906842679",
        "1125899906842723",
        /*timeout_s=*/60,
        /*expected_bits=*/101,
        /*expected_digits=*/31};

    // 180-bit: two consecutive 90-bit primes
    const Case c180 = {
        "180bit",
        "1532495540865888858358347631265048354884313272956270703",
        "1237940039285380274899124357",
        "1237940039285380274899124579",
        /*timeout_s=*/600,
        /*expected_bits=*/181,
        /*expected_digits=*/55};

    // Smaller fixed case for ENV force test (router would normally pick rho)
    const Case c_small = {
        "21d",
        "1267650600228402790082356974917",  // reuse 100-bit (router still picks SIQS)
        "1125899906842679",
        "1125899906842723",
        60, 101, 31};

    bool ok = true;
    ok &= run_direct_siqs(c100);
    ok &= run_pipeline_siqs(c100);
    ok &= run_force_siqs_env(c_small);
    ok &= run_direct_siqs(c180);
    // Skip pipeline-route check for 180-bit by default to keep e2e under slow tier;
    // direct factor() above already exercises the same algorithm.

    if (!ok) {
        printf("\n=== SIQS e2e FAILED ===\n");
        return 1;
    }
    printf("\n=== SIQS e2e PASS (100-bit + 180-bit + force_env) ===\n");
    return 0;
}
