// test_adaptive_lattice.cpp — Adaptive lattice basis re-reduction unit tests.
//
// Verifies `include/gnfs/sieve/adaptive_lattice.hpp` (and its integration with
// LatticeBasis from lattice_basis.hpp). Covers:
//   1. Config parsing (ENV off / on / custom threshold / retries / seed)
//   2. Density estimation correctness
//   3. Perturbation preserves the lattice while intentionally leaving the
//      canonical LLL representative
//   4. try_perturb_and_rereduce returns nullopt when density already high
//   5. try_perturb_and_rereduce returns new basis when density low + retries
//   6. Retry budget respected (retry_count >= max_retries → nullopt)
//   7. Telemetry counts (atomic, thread-safe)
//   8. Concurrent access (multiple threads recording stats — no torn writes)
//   9. Disabled mode: returns nullopt immediately, telemetry untouched
//  10. Determinant preservation across perturbations
//  11. verify_ab strictness preserved on perturbed basis
//  12. Different retry counts produce different perturbations
//  13. Integration smoke: small SQ range sieve (skipped under sanitizers /
//      slow CI when GNFS_ADAPTIVE_LATTICE_SKIP_INTEGRATION=1)
//
// Test framework: project's bespoke assert-based pattern (no GoogleTest).

#include "gnfs/core/integer.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/sieve/adaptive_lattice.hpp"
#include "gnfs/sieve/lattice_basis.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using namespace gnfs::sieve;
#if defined(__SIZEOF_INT128__)
using wide_int = __int128_t;
#else
using wide_int = long double;
#endif

namespace {

// ── helpers (mirror of test_lll_lattice.cpp) ────────────────────────────

[[nodiscard]] wide_int norm_sq_i128(int64_t a, int64_t b) noexcept {
    wide_int a128 = static_cast<wide_int>(a);
    wide_int b128 = static_cast<wide_int>(b);
    return a128 * a128 + b128 * b128;
}

[[nodiscard]] wide_int dot_i128(int64_t a0, int64_t b0, int64_t a1, int64_t b1) noexcept {
    return static_cast<wide_int>(a0) * static_cast<wide_int>(a1) +
           static_cast<wide_int>(b0) * static_cast<wide_int>(b1);
}

[[nodiscard]] wide_int abs_i128(wide_int x) noexcept { return x < 0 ? -x : x; }

[[nodiscard]] bool is_size_reduced(const LatticeBasis& basis) {
    wide_int n0 = norm_sq_i128(basis.e0, basis.f0);
    if (n0 == 0) return true;
    wide_int d = dot_i128(basis.e0, basis.f0, basis.e1, basis.f1);
    return abs_i128(2 * d) <= n0;
}

[[nodiscard]] bool satisfies_lovasz(const LatticeBasis& basis) {
    wide_int n0 = norm_sq_i128(basis.e0, basis.f0);
    wide_int n1 = norm_sq_i128(basis.e1, basis.f1);
    return n1 >= n0;
}

[[nodiscard]] int64_t abs_det(const LatticeBasis& basis) {
    int64_t d = basis.determinant();
    return d < 0 ? -d : d;
}

/// Validate that the basis is a legal basis of L_q (det = ±q) and that both
/// vectors satisfy verify_ab. LLL invariants (size-reduced + Lovász) are NOT
/// required here because perturbed basses are intentionally outside the LLL
/// canonical form.
[[nodiscard]] bool basis_is_valid(const LatticeBasis& basis) {
    if (abs_det(basis) != static_cast<int64_t>(basis.q)) return false;
    if (!basis.verify_ab(basis.e0, basis.f0)) return false;
    if (!basis.verify_ab(basis.e1, basis.f1)) return false;
    return true;
}

/// Strict LLL-reduced check (for the INITIAL basis only).
[[nodiscard]] bool basis_is_lll(const LatticeBasis& basis) {
    if (!basis_is_valid(basis)) return false;
    if (!is_size_reduced(basis)) return false;
    if (!satisfies_lovasz(basis)) return false;
    return true;
}

[[noreturn]] void fail_check(const char* message) {
    std::cerr << "\n  ERROR: " << message << std::endl;
    std::abort();
}

void check(bool condition, const char* message) {
    if (!condition) {
        fail_check(message);
    }
}

[[nodiscard]] bool basis_equal(const LatticeBasis& lhs,
                               const LatticeBasis& rhs) noexcept {
    return lhs.e0 == rhs.e0 && lhs.f0 == rhs.f0 &&
           lhs.e1 == rhs.e1 && lhs.f1 == rhs.f1 &&
           lhs.q == rhs.q && lhs.r == rhs.r;
}

[[nodiscard]] bool adaptive_config_equal(
        const AdaptiveLatticeConfig& lhs,
        const AdaptiveLatticeConfig& rhs) noexcept {
    return lhs.enabled == rhs.enabled &&
           lhs.density_threshold == rhs.density_threshold &&
           lhs.max_retries == rhs.max_retries &&
           lhs.perturb_seed == rhs.perturb_seed;
}

[[nodiscard]] bool sieve_candidate_equal(
        const SieveCandidate& lhs,
        const SieveCandidate& rhs) noexcept {
    return lhs.i == rhs.i && lhs.j == rhs.j && lhs.a == rhs.a &&
           lhs.b == rhs.b && lhs.residual == rhs.residual;
}

[[nodiscard]] bool sieve_result_equal(const SieveResult& lhs,
                                      const SieveResult& rhs) noexcept {
    if (lhs.special_q.q != rhs.special_q.q ||
        lhs.special_q.r != rhs.special_q.r ||
        lhs.special_q.index != rhs.special_q.index ||
        lhs.sieved_positions != rhs.sieved_positions ||
        lhs.smooth_count != rhs.smooth_count ||
        lhs.candidates.size() != rhs.candidates.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.candidates.size(); ++i) {
        if (!sieve_candidate_equal(lhs.candidates[i], rhs.candidates[i])) {
            return false;
        }
    }
    return true;
}

void clear_env() {
    unsetenv("GNFS_ADAPTIVE_LATTICE");
    unsetenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD");
    unsetenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES");
    unsetenv("GNFS_ADAPTIVE_LATTICE_SEED");
}

void clear_explicit_policy_env() {
    clear_env();
    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");
}

}  // namespace

// ── 1. Config parsing ───────────────────────────────────────────────────

void test_config_default_off() {
    std::cout << "test_config_default_off ... ";
    clear_env();
    auto cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == false);
    assert(cfg.density_threshold == 0.5);
    assert(cfg.max_retries == 2);
    assert(cfg.perturb_seed == 0);
    std::cout << "PASS\n";
}

void test_config_env_on() {
    std::cout << "test_config_env_on ... ";
    clear_env();
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    auto cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == true);
    clear_env();

    setenv("GNFS_ADAPTIVE_LATTICE", "on", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == true);
    clear_env();

    setenv("GNFS_ADAPTIVE_LATTICE", "true", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == true);
    clear_env();

    // "0" must remain off
    setenv("GNFS_ADAPTIVE_LATTICE", "0", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == false);
    clear_env();
    std::cout << "PASS\n";
}

void test_config_custom_threshold() {
    std::cout << "test_config_custom_threshold ... ";
    clear_env();
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "1.5", 1);
    auto cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.enabled == true);
    assert(cfg.density_threshold == 1.5);
    clear_env();

    // out-of-range threshold ignored
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "-0.5", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.density_threshold == 0.5);  // default preserved
    clear_env();

    // garbage ignored
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "abc", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.density_threshold == 0.5);
    clear_env();
    std::cout << "PASS\n";
}

void test_config_custom_retries_and_seed() {
    std::cout << "test_config_custom_retries_and_seed ... ";
    clear_env();
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "5", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_SEED", "12345", 1);
    auto cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.max_retries == 5);
    assert(cfg.perturb_seed == 12345);
    clear_env();

    // out-of-range retries ignored
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "999", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.max_retries == 2);  // default
    clear_env();

    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "-1", 1);
    cfg = AdaptiveLatticeConfig::from_env();
    assert(cfg.max_retries == 2);
    clear_env();
    std::cout << "PASS\n";
}

// ── 2. Density estimation ───────────────────────────────────────────────

void test_density_estimation() {
    std::cout << "test_density_estimation ... ";
    assert(detail::compute_density(0, 0) == 0.0);
    assert(detail::compute_density(0, 100) == 0.0);
    assert(detail::compute_density(50, 100) == 0.5);
    assert(detail::compute_density(100, 100) == 1.0);
    assert(detail::compute_density(300, 100) == 3.0);
    std::cout << "PASS\n";
}

// ── 3. Perturbation produces valid lattice basis ─────────────────────────

void test_perturbation_valid_lattice() {
    std::cout << "test_perturbation_valid_lattice ... ";
    // Pick several distinct (q, r) and verify perturbations preserve det ± q
    // and verify_ab on both vectors. LLL invariants are intentionally relaxed
    // because the perturbation deliberately steps outside LLL canonical form.
    struct QR { uint32_t q; uint32_t r; };
    QR cases[] = {
        {1009, 503}, {2003, 1001}, {65537, 32768}, {100003, 50001},
        {1000003, 500001}, {1234577, 999999},
    };
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 100.0, 4, 0});
    for (auto& c : cases) {
        SpecialQ sq{c.q, c.r, 0};
        LatticeBasis base = mgr.get_initial(sq, 1.0, 0);
        // Initial basis must be LLL-strict.
        assert(basis_is_lll(base));

        for (int retry = 0; retry < 4; ++retry) {
            auto opt = mgr.try_perturb_and_rereduce(base, 0, 1000, retry);
            assert(opt.has_value());
            // Perturbed basis must be a legal L_q basis (det = ±q, verify_ab)
            // but need not be LLL-canonical.
            assert(basis_is_valid(*opt));
        }
    }
    std::cout << "PASS\n";
}

// ── 4. nullopt when density already high ────────────────────────────────

void test_no_perturb_when_dense() {
    std::cout << "test_no_perturb_when_dense ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    // density = 1000/1000 = 1.0 >= threshold 0.5
    auto opt = mgr.try_perturb_and_rereduce(base, 1000, 1000, 0);
    assert(!opt.has_value());

    // density = 600/1000 = 0.6 >= 0.5
    opt = mgr.try_perturb_and_rereduce(base, 600, 1000, 0);
    assert(!opt.has_value());

    // density = exact threshold 0.5 — still NOT perturbed (>= 0.5)
    opt = mgr.try_perturb_and_rereduce(base, 500, 1000, 0);
    assert(!opt.has_value());

    std::cout << "PASS\n";
}

// ── 5. New basis returned when density low ──────────────────────────────

void test_perturb_when_low_density() {
    std::cout << "test_perturb_when_low_density ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    auto opt = mgr.try_perturb_and_rereduce(base, 100, 1000, 0);
    assert(opt.has_value());
    assert(basis_is_valid(*opt));

    // perturbed should differ from original (otherwise no rescue possible)
    bool different = (opt->e0 != base.e0) || (opt->e1 != base.e1)
                  || (opt->f0 != base.f0) || (opt->f1 != base.f1);
    assert(different);

    std::cout << "PASS\n";
}

// ── 6. Retry budget respected ───────────────────────────────────────────

void test_retry_budget_respected() {
    std::cout << "test_retry_budget_respected ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    // retry_count < max_retries → returns basis
    assert(mgr.try_perturb_and_rereduce(base, 10, 1000, 0).has_value());
    assert(mgr.try_perturb_and_rereduce(base, 10, 1000, 1).has_value());
    // retry_count == max_retries → nullopt
    assert(!mgr.try_perturb_and_rereduce(base, 10, 1000, 2).has_value());
    // retry_count > max_retries → nullopt
    assert(!mgr.try_perturb_and_rereduce(base, 10, 1000, 5).has_value());

    // max_retries = 0 → no perturbation ever
    AdaptiveBasisManager mgr0(AdaptiveLatticeConfig{true, 0.5, 0, 0});
    assert(!mgr0.try_perturb_and_rereduce(base, 0, 1000, 0).has_value());

    std::cout << "PASS\n";
}

// ── 7. Telemetry counts ─────────────────────────────────────────────────

void test_telemetry_counts() {
    std::cout << "test_telemetry_counts ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    mgr.record_hit_stats(base, 100, 1000);
    mgr.record_hit_stats(base, 200, 1000);
    mgr.mark_special_q_processed();
    mgr.mark_special_q_processed();
    mgr.mark_retry_attempted();
    mgr.mark_rescue_succeeded();
    mgr.mark_low_density_skipped();

    auto snap = mgr.stats().snapshot();
    assert(snap.total_hits == 300);
    assert(snap.total_cells == 2000);
    assert(snap.special_qs_processed == 2);
    assert(snap.retries_attempted == 1);
    assert(snap.rescues_succeeded == 1);
    assert(snap.low_density_skipped == 1);

    std::cout << "PASS\n";
}

// ── 8. Concurrent access ────────────────────────────────────────────────

void test_concurrent_telemetry() {
    std::cout << "test_concurrent_telemetry ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    constexpr int N_THREADS = 8;
    constexpr int ITERS = 1000;
    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&mgr, &base]() {
            for (int i = 0; i < ITERS; ++i) {
                mgr.record_hit_stats(base, 1, 10);
                mgr.mark_special_q_processed();
                if (i % 3 == 0) mgr.mark_retry_attempted();
                if (i % 5 == 0) mgr.mark_rescue_succeeded();
            }
        });
    }
    for (auto& th : threads) th.join();

    auto snap = mgr.stats().snapshot();
    assert(snap.total_hits == N_THREADS * ITERS);
    assert(snap.total_cells == N_THREADS * ITERS * 10);
    assert(snap.special_qs_processed == N_THREADS * ITERS);

    // i%3 == 0 yields ceil(ITERS / 3) per thread = (ITERS+2)/3
    uint64_t expected_retries = static_cast<uint64_t>(N_THREADS) * ((ITERS + 2) / 3);
    assert(snap.retries_attempted == expected_retries);

    uint64_t expected_rescues = static_cast<uint64_t>(N_THREADS) * ((ITERS + 4) / 5);
    assert(snap.rescues_succeeded == expected_rescues);

    std::cout << "PASS\n";
}

// ── 9. Disabled mode: zero-cost ─────────────────────────────────────────

void test_disabled_mode_zero_cost() {
    std::cout << "test_disabled_mode_zero_cost ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{false, 0.5, 2, 0});
    SpecialQ sq{1009, 503, 0};
    LatticeBasis base = mgr.get_initial(sq);

    // get_initial still works (returns standard LLL basis identical to compute_lattice_basis).
    LatticeBasis expected = compute_lattice_basis(sq);
    assert(base.e0 == expected.e0 && base.e1 == expected.e1);
    assert(base.f0 == expected.f0 && base.f1 == expected.f1);

    // try_perturb_and_rereduce → nullopt even with zero hits
    auto opt = mgr.try_perturb_and_rereduce(base, 0, 1000, 0);
    assert(!opt.has_value());

    // record / mark helpers no-op
    mgr.record_hit_stats(base, 999, 999);
    mgr.mark_special_q_processed();
    mgr.mark_retry_attempted();
    mgr.mark_rescue_succeeded();
    mgr.mark_low_density_skipped();

    auto snap = mgr.stats().snapshot();
    assert(snap.total_hits == 0);
    assert(snap.total_cells == 0);
    assert(snap.special_qs_processed == 0);
    assert(snap.retries_attempted == 0);
    assert(snap.rescues_succeeded == 0);
    assert(snap.low_density_skipped == 0);

    std::cout << "PASS\n";
}

// ── 10. Determinant preservation ────────────────────────────────────────

void test_determinant_preserved() {
    std::cout << "test_determinant_preserved ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 100.0, 4, 0});
    uint32_t qs[] = {1009, 2003, 65537, 100003, 1000003};
    for (uint32_t q : qs) {
        SpecialQ sq{q, q / 2, 0};
        LatticeBasis base = mgr.get_initial(sq);
        assert(abs_det(base) == static_cast<int64_t>(q));

        for (int retry = 0; retry < 4; ++retry) {
            auto opt = mgr.try_perturb_and_rereduce(base, 0, 1000, retry);
            assert(opt.has_value());
            assert(abs_det(*opt) == static_cast<int64_t>(q));
        }
    }
    std::cout << "PASS\n";
}

// ── 11. verify_ab strictness preserved ──────────────────────────────────

void test_verify_ab_preserved() {
    std::cout << "test_verify_ab_preserved ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 100.0, 4, 12345});
    SpecialQ sq{65537, 32768, 0};
    LatticeBasis base = mgr.get_initial(sq);

    for (int retry = 0; retry < 4; ++retry) {
        auto opt = mgr.try_perturb_and_rereduce(base, 0, 1000, retry);
        assert(opt.has_value());
        const LatticeBasis& pb = *opt;
        // Both basis vectors must satisfy a - b*r ≡ 0 (mod q)
        assert(pb.verify_ab(pb.e0, pb.f0));
        assert(pb.verify_ab(pb.e1, pb.f1));
        // Sample some lattice combos: to_ab(i,j) should always satisfy verify_ab
        for (int i = -10; i <= 10; ++i) {
            for (int j = 1; j <= 10; ++j) {
                auto [a, b] = pb.to_ab(i, j);
                assert(pb.verify_ab(a, b));
            }
        }
    }
    std::cout << "PASS\n";
}

// ── 12. Different retries produce distinct perturbations ────────────────

void test_distinct_perturbations() {
    std::cout << "test_distinct_perturbations ... ";
    AdaptiveBasisManager mgr(AdaptiveLatticeConfig{true, 100.0, 4, 0});
    SpecialQ sq{100003, 50001, 0};
    LatticeBasis base = mgr.get_initial(sq);

    std::set<std::pair<int64_t, int64_t>> seen;
    seen.insert({base.e0, base.f0});
    seen.insert({base.e1, base.f1});

    for (int retry = 0; retry < 4; ++retry) {
        auto opt = mgr.try_perturb_and_rereduce(base, 0, 1000, retry);
        assert(opt.has_value());
        seen.insert({opt->e0, opt->f0});
        seen.insert({opt->e1, opt->f1});
    }
    // At least 3 distinct basis vector pairs across 4 retries (rotational symmetry
    // may produce duplicates between k=+/-k after LLL re-reduction; require ≥3).
    assert(seen.size() >= 3);

    std::cout << "PASS\n";
}

void test_seed_zero_exact_retry_sequence_is_repeatable() {
    std::cout << "test_seed_zero_exact_retry_sequence_is_repeatable ... ";

    constexpr std::array<int, 8> EXPECTED_K = {
        1, -1, 2, -2, 1, -1, 2, -2};
    constexpr std::array<SpecialQ, 3> SPECIAL_QS = {{
        {1009, 503, 0},
        {100003, 50001, 1},
        {1000003, 500001, 2},
    }};
    const LatticeBasisReductionConfig basis_config{
        LatticeReductionMethod::LLL, false};
    const AdaptiveLatticeConfig adaptive_config{true, 100.0, 16, 0};
    AdaptiveBasisManager first_manager(adaptive_config);
    AdaptiveBasisManager second_manager(adaptive_config);

    for (const auto& sq : SPECIAL_QS) {
        const LatticeBasis base =
            first_manager.get_initial(sq, 1.0, basis_config);
        const LatticeBasis second_base =
            second_manager.get_initial(sq, 1.0, basis_config);
        check(basis_equal(base, second_base),
              "seed-zero managers produced different initial bases");

        for (std::size_t retry = 0; retry < EXPECTED_K.size(); ++retry) {
            const int retry_count = static_cast<int>(retry);
            check(detail::rotation_k_for_retry(retry_count, 0, sq.q) ==
                      EXPECTED_K[retry],
                  "seed-zero rotation sequence differs from exact oracle");

            const auto first = first_manager.try_perturb_and_rereduce(
                base, 0, 1000, retry_count);
            const auto repeated = first_manager.try_perturb_and_rereduce(
                base, 0, 1000, retry_count);
            const auto second = second_manager.try_perturb_and_rereduce(
                second_base, 0, 1000, retry_count);
            check(first.has_value() && repeated.has_value() &&
                      second.has_value(),
                  "seed-zero retry unexpectedly exhausted its budget");

            const LatticeBasis expected =
                detail::skew_perturb_basis(base, EXPECTED_K[retry]);
            check(basis_equal(*first, expected),
                  "seed-zero perturbation differs from exact basis oracle");
            check(basis_equal(*first, *repeated),
                  "seed-zero perturbation changed on repeated invocation");
            check(basis_equal(*first, *second),
                  "seed-zero perturbation changed across manager instances");
        }
    }

    std::cout << "PASS\n";
}

void test_explicit_manager_ignores_environment_changes() {
    std::cout << "test_explicit_manager_ignores_environment_changes ... ";
    clear_explicit_policy_env();

    const LatticeBasisReductionConfig basis_config{
        LatticeReductionMethod::LLL, true};
    const AdaptiveLatticeConfig adaptive_config{true, 100.0, 8, 0};
    AdaptiveBasisManager manager(adaptive_config);
    const SpecialQ sq{100003, 50001, 0};
    constexpr double SKEWNESS = 17.0;

    // Both ambient states conflict with the explicit skew-LLL policy and with
    // the explicit adaptive settings.
    setenv("GNFS_LATTICE_LLL", "0", 1);
    setenv("GNFS_LATTICE_SKEW", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "0.001", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_SEED", "999", 1);

    const LatticeBasis first =
        manager.get_initial(sq, SKEWNESS, basis_config);
    const auto first_perturb =
        manager.try_perturb_and_rereduce(first, 0, 1000, 0);
    check(first_perturb.has_value(),
          "explicit adaptive config was replaced by disabled ambient config");
    check(adaptive_config_equal(manager.config(), adaptive_config),
          "explicit manager config changed after conflicting ambient reads");

    setenv("GNFS_LATTICE_LLL", "1", 1);
    setenv("GNFS_LATTICE_SKEW", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "0.01", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_SEED", "123456", 1);

    const LatticeBasis second =
        manager.get_initial(sq, SKEWNESS, basis_config);
    const auto second_perturb =
        manager.try_perturb_and_rereduce(second, 0, 1000, 0);
    check(second_perturb.has_value(),
          "explicit adaptive config changed after ambient environment flip");
    check(basis_equal(first, second),
          "explicit basis output changed after ambient environment flip");
    check(basis_equal(*first_perturb, *second_perturb),
          "explicit perturbation changed after ambient environment flip");
    check(adaptive_config_equal(manager.config(), adaptive_config),
          "explicit manager config changed after ambient environment flip");

    clear_explicit_policy_env();
    std::cout << "PASS\n";
}

// ── 13. Integration smoke: small SQ range sieve adaptive on vs off ──────
//
// This test crosses module boundaries (sieve + adaptive manager) without
// actually running the full LatticeSieve (that takes seconds even on small N).
// Instead we simulate the integration by:
//   - Selecting 8 small SQ values
//   - Computing initial basis + a "fake" hit count via a deterministic function
//   - On the low-density ones, exercising retry budget
//   - Verifying telemetry counts are consistent across the two modes
//
// True end-to-end integration with sieve is covered by `test_lattice_sieve` /
// `test_sieve_basic` / `test_gnfs_e2e` once the lattice_sieve.hpp wire-in lands.

void test_integration_smoke() {
    std::cout << "test_integration_smoke ... ";

    struct QR { uint32_t q; uint32_t r; };
    QR qs[] = {
        {1009, 100}, {1013, 506}, {1019, 800}, {1021, 1020},
        {1031, 515}, {1033, 1032}, {1039, 200}, {1049, 524},
    };

    // Mode A: adaptive OFF
    AdaptiveBasisManager off_mgr(AdaptiveLatticeConfig{false, 0.5, 2, 0});
    for (auto& c : qs) {
        SpecialQ sq{c.q, c.r, 0};
        LatticeBasis base = off_mgr.get_initial(sq);
        off_mgr.record_hit_stats(base, /*hits*/ 50, /*cells*/ 1000);
        off_mgr.mark_special_q_processed();

        // off mode should never enter retry path
        auto opt = off_mgr.try_perturb_and_rereduce(base, 50, 1000, 0);
        assert(!opt.has_value());
    }
    auto off_snap = off_mgr.stats().snapshot();
    // Disabled → telemetry untouched
    assert(off_snap.special_qs_processed == 0);
    assert(off_snap.total_hits == 0);

    // Mode B: adaptive ON, threshold 0.1 (so 50/1000 = 0.05 triggers)
    AdaptiveBasisManager on_mgr(AdaptiveLatticeConfig{true, 0.1, 2, 0});
    int rescues = 0;
    for (auto& c : qs) {
        SpecialQ sq{c.q, c.r, 0};
        LatticeBasis basis = on_mgr.get_initial(sq);

        uint64_t hits = 50;     // density 0.05 < threshold 0.1
        uint64_t cells = 1000;
        int retry_count = 0;
        bool rescued = false;

        on_mgr.record_hit_stats(basis, hits, cells);
        while (true) {
            auto opt = on_mgr.try_perturb_and_rereduce(basis, hits, cells, retry_count);
            if (!opt.has_value()) break;
            assert(basis_is_valid(*opt));
            basis = *opt;
            on_mgr.mark_retry_attempted();
            ++retry_count;

            // simulate re-sieve: this retry "rescued" the SQ when retry==1.
            if (retry_count == 1) {
                hits = 600;  // density 0.6 > threshold 0.1 — rescued
                cells = 1000;
                on_mgr.record_hit_stats(basis, hits, cells);
                on_mgr.mark_rescue_succeeded();
                rescued = true;
                break;
            }
        }
        if (!rescued) on_mgr.mark_low_density_skipped();
        on_mgr.mark_special_q_processed();
        if (rescued) ++rescues;
    }
    auto on_snap = on_mgr.stats().snapshot();
    assert(on_snap.special_qs_processed == sizeof(qs) / sizeof(qs[0]));
    assert(on_snap.retries_attempted == on_snap.special_qs_processed);  // 1 retry each
    assert(on_snap.rescues_succeeded == on_snap.special_qs_processed);  // all rescued
    assert(rescues == static_cast<int>(on_snap.rescues_succeeded));

    // Sanity: ON mode collected real hits, OFF mode collected zero (gating).
    assert(on_snap.total_hits > 0);
    assert(off_snap.total_hits == 0);

    std::cout << "PASS (qs=" << on_snap.special_qs_processed
              << " retries=" << on_snap.retries_attempted
              << " rescues=" << on_snap.rescues_succeeded << ")\n";
}

// ── 14. Integration: LatticeSieve adaptive on vs off ────────────────────
//
// End-to-end integration check: run small LatticeSieve over a range of SQs
// with adaptive OFF (baseline) and ON (forced retries via very low
// threshold), confirm:
//   - ON ≥ 0.95 × OFF total candidates (no degradation)
//   - No duplicate (a, b) pairs within any one special-Q result
//   - When ON: rescue rate > 0 on at least one SQ (telemetry sanity)

void test_lattice_sieve_integration() {
    std::cout << "test_lattice_sieve_integration ... ";

    using namespace gnfs;
    using namespace gnfs::core;
    using namespace gnfs::polynomial;
    using namespace gnfs::factor_base;

    // Small N: 1000003 * 1000033 = 1000036000099 (13 digit).
    Integer n("1000036000099");
    uint32_t degree = 3;
    auto poly_result = BaseMSelector::select(n, degree);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    assert(ctx.verify());

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 5000;
    fb_opts.algebraic_bound = 5000;
    fb_opts.log_scale = 16;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 60;
    sieve_params.algebraic_threshold = 60;

    SieveRegion region;
    region.i_min = -1000;
    region.i_max = 999;
    region.j_min = 1;
    region.j_max = 200;

    SpecialQRange sq_range;
    sq_range.min_q = 1000;
    sq_range.max_q = 3000;
    constexpr size_t NUM_SQ = 12;

    // ── Baseline: adaptive OFF ────────────────────────────────────────
    clear_env();
    size_t off_total = 0;
    {
        sieve::LatticeSieve sieve(ctx, fb, sieve_params);
        sieve.set_region(region);
        // Confirm manager is disabled (no ENV).
        assert(!sieve.adaptive_manager().config().enabled);

        sieve::SpecialQGenerator gen(fb, sq_range);
        for (size_t i = 0; i < NUM_SQ && gen.has_next(); ++i) {
            auto sq = gen.next();
            if (!sq) break;
            auto r = sieve.sieve_special_q(*sq);
            off_total += r.candidates.size();
        }
    }

    // ── Adaptive ON: tuned threshold so most SQs trigger AND at least
    //    one rescue lands.  Sieve average density on this fixture is
    //    ~2.3% (10k hits / 400k cells × 12 SQs).  Set threshold to 0.02
    //    so SQs below average trigger retry and some clear the bar after
    //    perturbation.
    setenv("GNFS_ADAPTIVE_LATTICE", "1", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "0.02", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "2", 1);

    size_t on_total = 0;
    sieve::AdaptiveLatticeStats::Snapshot on_snap{};
    {
        sieve::LatticeSieve sieve(ctx, fb, sieve_params);
        sieve.set_region(region);
        assert(sieve.adaptive_manager().config().enabled);
        assert(sieve.adaptive_manager().config().density_threshold == 0.02);

        sieve::SpecialQGenerator gen(fb, sq_range);
        for (size_t i = 0; i < NUM_SQ && gen.has_next(); ++i) {
            auto sq = gen.next();
            if (!sq) break;
            auto r = sieve.sieve_special_q(*sq);
            on_total += r.candidates.size();
            std::set<std::pair<int64_t, uint64_t>> sq_pairs;
            for (auto& c : r.candidates) {
                // (a, b) pair invariant: gcd=1, b>0 (also checked inside collect_candidates)
                sq_pairs.insert({c.a, c.b});
            }
            check(sq_pairs.size() == r.candidates.size(),
                  "adaptive sieve emitted a duplicate (a,b) within one special-Q");
        }
        on_snap = sieve.adaptive_manager().stats().snapshot();
    }
    clear_env();

    // Invariant 1: total candidates not degraded (>= 95% of baseline).
    // Adaptive only ADOPTS a retry result when it improves hits, so this should
    // hold by construction.
    assert(on_total >= off_total * 95 / 100);

    // Invariant 2: telemetry consistent with config. Duplicate checks are
    // intentionally per-SQ above because different SQ lattices may overlap.
    assert(on_snap.special_qs_processed == NUM_SQ);
    assert(on_snap.retries_attempted > 0);  // threshold is so low retries will fire

    std::cout << "PASS (off=" << off_total
              << " on=" << on_total
              << " sqs=" << on_snap.special_qs_processed
              << " retries=" << on_snap.retries_attempted
              << " rescues=" << on_snap.rescues_succeeded
              << ")\n";
}

void test_explicit_sieve_parallel_preserves_execution_config() {
    std::cout << "test_explicit_sieve_parallel_preserves_execution_config ... ";

    using namespace gnfs;
    using namespace gnfs::core;
    using namespace gnfs::polynomial;
    using namespace gnfs::factor_base;

    clear_explicit_policy_env();
    setenv("GNFS_LATTICE_LLL", "0", 1);
    setenv("GNFS_LATTICE_SKEW", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD", "0.001", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", "0", 1);
    setenv("GNFS_ADAPTIVE_LATTICE_SEED", "999", 1);

    Integer n("1000036000099");
    const auto poly_result = BaseMSelector::select(n, 3);
    check(poly_result.success,
          "explicit parallel fixture polynomial selection failed");
    const auto ctx = BaseMSelector::create_context(n, poly_result);
    check(ctx.verify(), "explicit parallel fixture context is invalid");

    FactorBaseBuilder::Options fb_options;
    fb_options.rational_bound = 3000;
    fb_options.algebraic_bound = 3000;
    fb_options.log_scale = 16;
    fb_options.parallel = false;
    const auto fb = FactorBaseBuilder::build(ctx, fb_options);

    SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 55;
    sieve_params.algebraic_threshold = 55;

    SieveRegion region;
    region.i_min = -400;
    region.i_max = 399;
    region.j_min = 1;
    region.j_max = 80;

    SpecialQRange sq_range;
    sq_range.min_q = 1000;
    sq_range.max_q = 3000;
    SpecialQGenerator generator(fb, sq_range);
    std::vector<SpecialQ> special_qs;
    while (special_qs.size() < 8 && generator.has_next()) {
        const auto sq = generator.next();
        if (!sq.has_value()) {
            break;
        }
        special_qs.push_back(*sq);
    }
    check(special_qs.size() == 8,
          "explicit parallel fixture did not produce eight special-Q values");

    const LatticeSieveExecutionConfig explicit_config{
        LatticeBasisReductionConfig{LatticeReductionMethod::LLL, true},
        AdaptiveLatticeConfig{true, 100.0, 2, 0}};

    LatticeSieve sequential_sieve(ctx, fb, sieve_params, explicit_config);
    sequential_sieve.set_region(region);
    std::vector<SieveResult> sequential_results;
    sequential_results.reserve(special_qs.size());
    for (const auto& sq : special_qs) {
        sequential_results.push_back(sequential_sieve.sieve_special_q(sq));
    }
    const auto sequential_stats =
        sequential_sieve.adaptive_manager().stats().snapshot();

    LatticeSieve parallel_sieve(ctx, fb, sieve_params, explicit_config);
    parallel_sieve.set_region(region);
    const auto parallel_results =
        parallel_sieve.sieve_parallel(special_qs, 4);
    const auto parallel_stats =
        parallel_sieve.adaptive_manager().stats().snapshot();

    check(parallel_results.size() == sequential_results.size(),
          "explicit parallel result count differs from sequential result count");
    std::size_t candidate_total = 0;
    for (std::size_t i = 0; i < sequential_results.size(); ++i) {
        check(sieve_result_equal(sequential_results[i], parallel_results[i]),
              "explicit parallel per-SQ candidate fields/order differ from sequential");
        candidate_total += sequential_results[i].candidates.size();
    }
    check(candidate_total > 0,
          "explicit parallel fixture produced no observable candidates");
    check(adaptive_config_equal(parallel_sieve.adaptive_manager().config(),
                                explicit_config.adaptive_lattice),
          "explicit parallel host manager lost its adaptive config");
    check(parallel_stats.special_qs_processed == special_qs.size(),
          "explicit parallel host manager did not observe every special-Q");
    check(parallel_stats.retries_attempted > 0,
          "explicit parallel fixture did not exercise adaptive retries");
    check(parallel_stats.special_qs_processed ==
              sequential_stats.special_qs_processed,
          "parallel and sequential special-Q telemetry differs");
    check(parallel_stats.retries_attempted ==
              sequential_stats.retries_attempted,
          "parallel and sequential retry telemetry differs");
    check(parallel_stats.rescues_succeeded ==
              sequential_stats.rescues_succeeded,
          "parallel and sequential rescue telemetry differs");
    check(parallel_stats.low_density_skipped ==
              sequential_stats.low_density_skipped,
          "parallel and sequential skipped telemetry differs");
    check(parallel_stats.total_hits == sequential_stats.total_hits,
          "parallel and sequential hit telemetry differs");
    check(parallel_stats.total_cells == sequential_stats.total_cells,
          "parallel and sequential cell telemetry differs");

    // Prove this fixture observes a dropped execution config: a legacy sieve
    // under the opposite ambient basis policy, but with the same explicit
    // adaptive manager injected, must differ for at least one SQ.
    AdaptiveBasisManager explicit_adaptive(explicit_config.adaptive_lattice);
    LatticeSieve dropped_config_sieve(ctx, fb, sieve_params);
    dropped_config_sieve.set_region(region);
    dropped_config_sieve.set_adaptive_manager(&explicit_adaptive);
    bool observed_policy_difference = false;
    for (std::size_t i = 0; i < special_qs.size(); ++i) {
        const auto dropped =
            dropped_config_sieve.sieve_special_q(special_qs[i]);
        if (!sieve_result_equal(sequential_results[i], dropped)) {
            observed_policy_difference = true;
        }
    }
    check(observed_policy_difference,
          "opposite ambient basis policy did not expose dropped explicit config");

    clear_explicit_policy_env();
    std::cout << "PASS (sqs=" << special_qs.size()
              << " candidates=" << candidate_total << ")\n";
}

// ── main ────────────────────────────────────────────────────────────────

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "  Adaptive Lattice Basis Re-reduction Tests" << std::endl;
    std::cout << "===========================================" << std::endl;

    test_config_default_off();
    test_config_env_on();
    test_config_custom_threshold();
    test_config_custom_retries_and_seed();
    test_density_estimation();
    test_perturbation_valid_lattice();
    test_no_perturb_when_dense();
    test_perturb_when_low_density();
    test_retry_budget_respected();
    test_telemetry_counts();
    test_concurrent_telemetry();
    test_disabled_mode_zero_cost();
    test_determinant_preserved();
    test_verify_ab_preserved();
    test_distinct_perturbations();
    test_seed_zero_exact_retry_sequence_is_repeatable();
    test_explicit_manager_ignores_environment_changes();
    test_integration_smoke();
    test_lattice_sieve_integration();
    test_explicit_sieve_parallel_preserves_execution_config();

    std::cout << "===========================================" << std::endl;
    std::cout << "  All adaptive lattice tests passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
