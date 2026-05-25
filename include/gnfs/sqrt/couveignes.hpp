#pragma once

#include "number_field.hpp"
#include "modular_poly.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../util/bit_intrin.hpp"
#include "../util/primes.hpp"

#include <vector>
#include <optional>
#include <iostream>

namespace gnfs::sqrt {

using core::Integer;
using core::PolynomialContext;

/// Couveignes square root configuration
struct CouveignesSqrtConfig {
    // Sign determination searches 2^num_primes patterns (up to 2^20 ≈ 1M)
    // More primes = larger CRT modulus = handles larger sqrt coefficients
    size_t num_primes = 16;        // Number of primes for CRT
    uint64_t prime_start = 1000;   // Starting point for prime search (larger = better)
    size_t max_attempts = 100;     // Max attempts for sign resolution
    size_t max_prime_checks = 100000;  // Max prime candidates to check (prevents infinite loop)

    // ── Large class group support (2026-05-21) ──
    // Number of extra quadratic characters to verify each candidate sign pattern.
    // Set to 0 to disable character verification (legacy behavior). When > 0,
    // each candidate Y(α) is first tested against K character constraints
    // (cheap O(d) per character) before the expensive Y² ≡ X² check. This
    // catches false sign patterns that arise from nontrivial class group
    // 2-torsion (the source of "large class group failure" in legacy code).
    size_t num_characters = 0;     // 0 = disabled (legacy); 8 = balanced; 16 = strict
    uint64_t character_prime_start = 10007;  // Distinct from CRT prime_start
    size_t max_character_prime_checks = 50000;  // Bound character prime search

    // Extra sign bits beyond 2^num_primes Gray code. Multiplies the pattern
    // space by 2^extra_sign_bits. Currently informational only — extension
    // hook for future "twist-element" search if class group 2-rank > 0.
    // Capped at 4 (2^20 total pattern budget when num_primes=16).
    size_t extra_sign_bits = 0;
};

/// Per-call telemetry from CouveignesSqrt. Populated by compute() /
/// compute_from_element() regardless of success/failure. Optional ENV
/// `GNFS_COUVEIGNES_VERBOSE=1` emits this to stderr.
///
/// Hot-path overhead: ~10 integer increments per call (negligible vs the
/// 65536-pattern Gray-code search and per-prime Tonelli-Shanks).
struct CouveignesMetrics {
    // Prime selection counters
    size_t primes_checked = 0;
    size_t primes_used = 0;
    size_t primes_skipped_divides_n = 0;
    size_t primes_skipped_bad_leading = 0;
    size_t primes_skipped_reducible = 0;
    size_t primes_skipped_zero_product = 0;
    size_t primes_skipped_no_sqrt = 0;
    size_t primes_skipped_ramified = 0;  // disc(f) ≡ 0 mod p — added 2026-05-21

    // Sign-pattern search counters
    size_t sign_patterns_tried = 0;          // Gray code iterations
    size_t character_filter_rejects = 0;     // Patterns rejected by cheap char check
    size_t full_verifications = 0;           // Y² ≡ X² evaluations
    bool found_sqrt = false;

    // Character verification telemetry
    size_t character_primes_used = 0;        // K from config
    size_t character_primes_checked = 0;     // Search overhead

    // Reset all fields to default (used by compute() at entry)
    void reset() noexcept { *this = CouveignesMetrics{}; }
};

/// CouveignesSqrt - Compute algebraic square root using Couveignes method
///
/// The algorithm works by:
/// 1. Computing square roots modulo several small primes p
/// 2. Using CRT to lift the result
/// 3. Recognizing the coefficients as rational numbers
class CouveignesSqrt {
public:
    using Config = CouveignesSqrtConfig;
    using Metrics = CouveignesMetrics;

    CouveignesSqrt() : config_() {}

    explicit CouveignesSqrt(const Config& config)
        : config_(config) {}

    /// Telemetry from the most recent compute() / compute_from_element() call.
    /// Populated whether the call succeeded or returned nullopt.
    [[nodiscard]] const Metrics& last_metrics() const noexcept { return metrics_; }

    /// Compute square root of product of (a_i - b_i * alpha) elements.
    /// Note: GNFS convention is `a - b*α` (see CLAUDE.md "元素表示"),
    /// implementation in compute_product_mod_p() uses (a - b*x) factors.
    /// @param ab_pairs Vector of (a, b) pairs whose product's sqrt we want
    /// @param nf Number field
    /// @param apply_f_prime_correction true: 乘 f'(α)² 走 Thomé 标准路径
    ///        (caller 必须除 f'(m) mod N);false: 直接对 ∏(a-bα) 开方,
    ///        仅当 sqrt(∏) ∈ Z[α] 时数学正确 (小 N、简单类群)。当
    ///        gcd(f'(m), N) > 1 时 caller 切到 false 路径,避免 inversion
    ///        失败导致整个 dependency 丢失 (v19 fix for v18 regression)。
    /// @return Square root as number field element, or empty if failed.
    ///   Returns nullopt for empty input — the linear-algebra layer should
    ///   never produce an empty dependency, and silently returning 1 would
    ///   mask a downstream bug (caller would compute trivial gcd(±1, N)).
    [[nodiscard]] std::optional<NumberFieldElement> compute(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            bool apply_f_prime_correction = true) const {

        metrics_.reset();
        if (ab_pairs.empty()) {
            return std::nullopt;
        }

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Get polynomial coefficients mod p — mpz_fdiv_ui returns
        // floor-div remainder ∈ [0, p-1] (zero alloc per coefficient)
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                f[i] = static_cast<uint64_t>(mpz_fdiv_ui(nf.coeff(i).get_mpz(), p));
            }
            return f;
        };

        // Compute f'(x) coefficients mod p.
        // f'(x) = Σ_{i=1}^d i·f[i]·x^(i-1)
        // 用于 f'(α)² · S(α) 修正(见 compute_product_mod_p 注释)
        auto get_f_prime_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> fp(d > 0 ? d : 1, 0);
            // hoist p_int + coeff_buf out of d iter loop
            const Integer p_int(p);
            Integer coeff_buf;
            for (uint32_t i = 1; i <= d; ++i) {
                coeff_buf = nf.coeff(i);  // mpz_set into reused buffer
                coeff_buf *= static_cast<int64_t>(i);  // mpz_mul_si direct
                coeff_buf %= p_int;
                if (coeff_buf.is_negative()) {
                    coeff_buf += p_int;
                }
                fp[i - 1] = coeff_buf.to_uint64();
            }
            // 去除前导 0
            while (fp.size() > 1 && fp.back() == 0) fp.pop_back();
            return fp;
        };

        // Collect suitable primes. Reserve config_.num_primes (the exit criterion).
        std::vector<uint64_t> primes;
        primes.reserve(config_.num_primes);
        std::vector<std::vector<uint64_t>> sqrt_coeffs;  // sqrt coeffs mod each prime
        sqrt_coeffs.reserve(config_.num_primes);

        // Debug counters
        size_t primes_checked = 0;
        size_t primes_dividing_n = 0;
        size_t primes_bad_leading = 0;
        size_t primes_reducible = 0;
        size_t primes_zero_product = 0;
        size_t primes_no_sqrt = 0;
        size_t primes_ramified = 0;

        uint64_t p = config_.prime_start;
        while (primes.size() < config_.num_primes && primes_checked < config_.max_prime_checks) {
            p = next_prime(p);
            primes_checked++;

            // Skip primes that divide N — direct GMP divisibility (zero alloc)
            if (mpz_divisible_ui_p(n.get_mpz(), p)) {
                primes_dividing_n++;
                continue;
            }

            // Get f mod p
            auto f_mod_p = get_f_mod_p(p);

            // Check that f doesn't degenerate (leading coeff nonzero)
            if (f_mod_p.back() == 0) {
                primes_bad_leading++;
                continue;
            }

            // Full Rabin irreducibility test (not just "no roots")
            // For degree > 3, "no roots" ≠ "irreducible"
            if (!ModularPoly::is_irreducible(f_mod_p, p)) {
                primes_reducible++;
                continue;
            }

            // Compute product mod p
            auto product = compute_product_mod_p(ab_pairs, f_mod_p, p);

            if (product.is_zero()) {
                // Product is zero mod p, skip this prime
                primes_zero_product++;
                continue;
            }

            // ── f'(α)² 修正 (Thomé 2008, "Square Root Algorithms for the NFS") ──
            // T(α)² = f'(α)² · S(α) — 保证 T(α) ∈ Z[α] 即使 sqrt(S(α)) ∈ O_K \ Z[α]。
            // 在每个素数 p 上,我们改为对 (f'(x)² · S(α)) mod p 取 sqrt。
            // CRT 后得 T(α) ≡ f'(α) · √S(α);caller 评估 T(m) 后乘 inv(f'(m)) mod N
            // 还原真正的 sqrt 在 Z/NZ 中的表示。
            //
            // v19: apply_f_prime_correction=false 时跳过 (小 N + gcd(f'(m),N)>1
            // 场景,inv 不存在; sqrt(∏) 通常恰好在 Z[α], 老路径可用)。
            if (apply_f_prime_correction) {
                auto f_prime_mod_p = get_f_prime_mod_p(p);
                ModularPoly f_prime(f_prime_mod_p);
                if (f_prime.is_zero()) {
                    // f' ≡ 0 mod p — ramified prime (p divides every i·f[i],
                    // implies p divides disc(f) modulo Wronskian). Cannot use
                    // f'(α)² correction; counted separately for diagnostic
                    // visibility into class-group-related failures.
                    primes_ramified++;
                    continue;
                }
                ModularPoly f_prime_sq = ModularPoly::mul(f_prime, f_prime, f_mod_p, p);
                product = ModularPoly::mul(product, f_prime_sq, f_mod_p, p);
            }

            // Check if product is a square
            if (!ModularPoly::is_square(product, f_mod_p, p)) {
                primes_no_sqrt++;
                continue;
            }

            // Compute square root
            auto sqrt_p = ModularPoly::sqrt_tonelli_shanks(product, f_mod_p, p);

            if (sqrt_p.is_zero() && !product.is_zero()) {
                // Square root doesn't exist mod p (shouldn't happen for valid dependency)
                primes_no_sqrt++;
                continue;
            }

            // Verify: sqrt^2 should equal product mod p
            auto sqrt_squared_p = ModularPoly::mul(sqrt_p, sqrt_p, f_mod_p, p);
            bool sqrt_valid = true;
            for (size_t i = 0; i <= std::max(static_cast<size_t>(sqrt_squared_p.degree()),
                                             static_cast<size_t>(product.degree())); ++i) {
                if (sqrt_squared_p.coeff(i) != product.coeff(i)) {
                    sqrt_valid = false;
                    break;
                }
            }
            if (!sqrt_valid) {
                primes_no_sqrt++;
                continue;
            }

            // Store this prime and its square root coefficients (without sign normalization)
            // We'll handle sign consistency during CRT phase
            primes.push_back(p);
            std::vector<uint64_t> coeffs(d, 0);
            for (size_t i = 0; i < d && i <= static_cast<size_t>(sqrt_p.degree()); ++i) {
                coeffs[i] = sqrt_p.coeff(i);
            }
            sqrt_coeffs.push_back(std::move(coeffs));

            if (primes.size() >= config_.num_primes) {
                break;
            }
        }

        if (primes.size() < 2) {
            return std::nullopt;  // Not enough primes found
        }

        // Debug can be enabled via COUVEIGNES_DEBUG
        #ifdef COUVEIGNES_DEBUG
        std::cerr << "[Couveignes] primes=" << primes.size()
                  << " checked=" << primes_checked
                  << " reducible=" << primes_reducible
                  << " no_sqrt=" << primes_no_sqrt
                  << " zero_prod=" << primes_zero_product << "\n";
        #endif

        // Sign determination using subset enumeration
        // Enumerate all 2^k sign combinations for first k primes

        // Compute CRT modulus for all primes (Couveignes primes ≤ uint32 max)
        Integer M(1);
        for (uint64_t prime : primes) {
            M *= static_cast<int64_t>(prime);  // mpz_mul_si direct
        }

        // Publish into metrics. Local counters retained for cheap increment
        // in hot loop (avoids member-write false sharing if compute() ever
        // becomes threaded).
        metrics_.primes_checked = primes_checked;
        metrics_.primes_used = primes.size();
        metrics_.primes_skipped_divides_n = primes_dividing_n;
        metrics_.primes_skipped_bad_leading = primes_bad_leading;
        metrics_.primes_skipped_reducible = primes_reducible;
        metrics_.primes_skipped_zero_product = primes_zero_product;
        metrics_.primes_skipped_no_sqrt = primes_no_sqrt;
        metrics_.primes_skipped_ramified = primes_ramified;

        // Note: old compute_crt_with_signs lambda removed — replaced by
        // precomputed weights + Gray code incremental update below

        // === OPTIMIZED sign pattern search ===
        // Key optimizations:
        // 1. Precompute CRT weights once (M_j * M_j_inv for each prime j)
        // 2. Compute base CRT (all-positive signs), then use Gray code enumeration
        //    so each step only flips one sign → incremental CRT update in O(d)
        // 3. Early rejection: check first coefficient before full d² verification

        // Exhaustive search over sign patterns (2^k where k = #primes).
        // Cap at 16 primes (65536 patterns) — beyond 16 the search is
        // truncated and primes 17+ are silently held positive,which can
        // miss the correct sign combination. We assert here so config that
        // requests >16 primes fails loudly rather than silently degrading.
        // If you need >16 primes for precision,either:
        //   (a) increase prime sizes so 16 of them suffice,or
        //   (b) implement Gray-code over the full K-1 bit space with
        //       verify short-circuit (see hensel_sqrt.hpp for the pattern).
        if (primes.size() > 16) {
            throw std::logic_error(
                "Couveignes::compute: >16 primes not supported in 65536-pattern search; "
                "either reduce num_primes or extend the search to K-1 sign bits");
        }
        size_t num_to_search = primes.size();
        uint64_t max_patterns = 1ULL << num_to_search;

        // --- Step 1: Precompute CRT weights ---
        // weight[j][i] = c_ij * M_j * M_j_inv mod M  (for each prime j, coefficient i)
        // When sign[j] is flipped, we subtract 2 * weight[j][i] from coeff[i]
        std::vector<std::vector<Integer>> weights(primes.size());

        // v22: M_j / M_j_mod_pj 复用 across j iter + w hoist + int64_t direct
        Integer M_j, M_j_mod_pj;
        Integer w;  // hoist per (j, i) iter
        for (size_t j = 0; j < primes.size(); ++j) {
            uint64_t p_j = primes[j];
            M_j = M;
            M_j /= int64_t(p_j);
            M_j_mod_pj = M_j;
            M_j_mod_pj %= int64_t(p_j);
            uint64_t M_j_inv = mod_inverse_u64(M_j_mod_pj.to_uint64(), p_j);

            weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                w = uint64_t(sqrt_coeffs[j][i]);  // mpz_set_ui (no init)
                w *= M_j;
                w *= static_cast<int64_t>(M_j_inv);
                w %= M;
                weights[j][i] = w;  // mpz_set into resized slot (skip tmp clone)
            }
        }

        // --- Step 2: Compute base CRT (all signs = +) ---
        std::vector<Integer> base_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            Integer coeff_i;  // default ctor = 0
            for (size_t j = 0; j < primes.size(); ++j) {
                coeff_i += weights[j][i];
            }
            coeff_i %= M;
            base_coeffs[i] = std::move(coeff_i);
        }

        // Center around 0 — v22: half_M 直接 assign
        Integer half_M;
        half_M = M;
        mpz_tdiv_q_2exp(half_M.get_mpz(), half_M.get_mpz(), 1);
        for (auto& c : base_coeffs) {
            if (c.compare(half_M) > 0) c -= M;
        }

        // Precompute 2 * weight[j][i] for incremental updates
        // v22: two_weights[j][i] = weights[j][i] (mpz_set into default-init)
        std::vector<std::vector<Integer>> two_weights(primes.size());
        for (size_t j = 0; j < primes.size(); ++j) {
            two_weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                // mpz_mul_2exp = bit shift, fastest power-of-2 multiply
                mpz_mul_2exp(two_weights[j][i].get_mpz(), weights[j][i].get_mpz(), 1);
            }
        }

        // --- Step 3: Gray code enumeration ---
        // current_coeffs starts as base (all-positive)
        // Each Gray code step flips exactly one bit
        // v22: current_coeffs[i] = base_coeffs[i] (mpz_set)
        std::vector<Integer> current_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            current_coeffs[i] = base_coeffs[i];
        }

        // Precompute expected_X2 = f'(m)² · ∏(a_i - b_i·m) mod N
        //
        // Couveignes 在每素数 p 上对 f'(α)² · S(α) 取 sqrt (compute_product_mod_p
        // 之后立即乘 f'(α)²),所以 Y(α) = f'(α)·sqrt(S(α));评估 Y(m) ≡
        // f'(m)·sqrt(S)(m) mod N,故 Y² ≡ f'(m)²·S(m) mod N。verify 必须比较
        // f'(m)² · expected_X2,否则全 search space 都对不上。
        //
        // 注意: 直接对 expected_X2 取 sqrt 等价于因子化 N,因此我们验证 Y² 而非 Y。
        // hot loop (10K+ iters per dep): term_buf = a - m*b via mpz_submul_ui
        Integer expected_X2(1);
        Integer term_buf;
        for (const auto& [a, b] : ab_pairs) {
            term_buf = a;  // mpz_set_si direct
            mpz_submul_ui(term_buf.get_mpz(), nf.m().get_mpz(),
                          static_cast<unsigned long>(b));
            term_buf %= n;
            if (term_buf.is_negative()) term_buf += n;
            expected_X2 *= term_buf;
            expected_X2 %= n;
        }
        // 乘 f'(m)² mod N (仅 apply_f_prime_correction=true 时)
        if (apply_f_prime_correction) {
            // f'(m) = Σ_{i=1}^d i · f[i] · m^(i-1)
            // mpz_addmul_ui: f_prime_m += nf.coeff(i)*i (fused FMA, i ≥ 1)
            const Integer& m_val = nf.m();
            Integer f_prime_m;  // default ctor = 0
            for (int i = static_cast<int>(d); i >= 1; --i) {
                f_prime_m *= m_val;
                mpz_addmul_ui(f_prime_m.get_mpz(),
                              nf.coeff(static_cast<uint32_t>(i)).get_mpz(),
                              static_cast<unsigned long>(i));
                f_prime_m %= n;
            }
            if (f_prime_m.is_negative()) f_prime_m += n;
            // f_prime_m_sq = f_prime_m^2 mod n via mpz_powm_ui
            Integer f_prime_m_sq;
            mpz_powm_ui(f_prime_m_sq.get_mpz(), f_prime_m.get_mpz(), 2, n.get_mpz());
            expected_X2 *= f_prime_m_sq;
            expected_X2 %= n;
        }

        // m^j mod N 缓存,Gray code 内每次 verify 不再重算
        // mpz_mul writes mpow[j-1]*m directly into mpow[j] (skip set)
        std::vector<Integer> mpow(d);
        mpow[0] = int64_t(1);  // mpz_set_si direct
        for (uint32_t j = 1; j < d; ++j) {
            mpz_mul(mpow[j].get_mpz(), mpow[j-1].get_mpz(), nf.m().get_mpz());
            mpow[j] %= n;
        }

        // ── Character verification setup (2026-05-21) ──
        //
        // For each character prime q (distinct from CRT primes):
        //   - Build f_q = f mod q (full polynomial).
        //   - Compute target_q = ∏(a - b·α) · f'(α)² mod (f_q, q)
        //     as a polynomial in F_q[x]/(f mod q), once at setup.
        //   - For each candidate Y in the Gray-code search:
        //     Reduce current_coeffs mod q to get Y_q ∈ F_q[x] (degree < d),
        //     compute Y_q² mod (f_q, q) via polynomial squaring + reduction,
        //     compare coefficient-by-coefficient against target_q.
        //   - Reject candidate if any character mismatches.
        //
        // Mathematical correctness: Y(α) ∈ Z[α] computed via CRT lifts
        // satisfies Y² ≡ f'² · S in Z[α]/(M). Reducing this congruence
        // mod q (any q coprime to disc(f)) gives Y² ≡ f'² · S in F_q[x]/(f)
        // because both sides are full polynomials in α with integer
        // coefficients, and the relation holds at the polynomial level
        // (not just at α = r_q). The M-dependent term that breaks the
        // Horner-at-r_q variant lives in the kernel of the polynomial
        // reduction mod q AS A POLYNOMIAL (i.e., the M-multiple of f(α)
        // becomes M-multiple of zero in F_q[x]/(f mod q)).
        //
        // Cost per character per pattern: O(d²) F_q multiplications, vs
        // O(d² + d · log q) for Horner-at-r_q + Legendre. For d=4-6,
        // ~25-50 uint64 ops per character. With K=8 characters and 65536
        // patterns: ~13M F_q ops per dependency, dwarfed by GMP Y² mod N
        // at log_2(N)=164 (~4000 ops/pattern, 262M ops total).
        //
        // CouveignesMetrics::character_primes_used reports collected
        // primes; character_filter_rejects counts patterns the filter
        // discards before full verification.
        struct CharacterPrime {
            uint64_t q;
            std::vector<uint64_t> f_q;      // f mod q (full polynomial, d+1 coeffs)
            std::vector<uint64_t> target_q; // S_q · f'_q² mod (f_q), d coeffs
        };
        std::vector<CharacterPrime> char_primes;
        char_primes.reserve(config_.num_characters);

        size_t char_primes_checked = 0;
        if (config_.num_characters > 0) {
            uint64_t q_cand = config_.character_prime_start;
            std::vector<uint64_t> crt_prime_set = primes;  // copy for overlap search

            while (char_primes.size() < config_.num_characters &&
                   char_primes_checked < config_.max_character_prime_checks) {
                q_cand = next_prime(q_cand);
                ++char_primes_checked;

                // Skip primes that divide N
                if (mpz_divisible_ui_p(n.get_mpz(), q_cand)) continue;

                // Skip primes already used in CRT (would correlate sign info)
                bool overlap = false;
                for (uint64_t p_crt : crt_prime_set) {
                    if (p_crt == q_cand) { overlap = true; break; }
                }
                if (overlap) continue;

                // Build f mod q (full polynomial, used for poly reduction)
                std::vector<uint64_t> f_q(d + 1);
                for (uint32_t i = 0; i <= d; ++i) {
                    f_q[i] = static_cast<uint64_t>(
                        mpz_fdiv_ui(nf.coeff(i).get_mpz(), q_cand));
                }
                if (f_q.back() == 0) continue;

                // Quick irreducibility filter — skip primes where f mod q
                // factors. This is desirable because the character check is
                // strictly tighter when F_q[x]/(f mod q) is a field. Not
                // strictly required (the check still works for split primes
                // because the target is a polynomial, not a Legendre bit).
                //
                // OPTIONAL: Removed the irreducibility test to allow split
                // primes; the poly mul check works correctly either way.

                // Build target_q = S(α) · f'(α)² mod (f_q, q) as a poly in F_q[x].
                // Start with constant 1, multiply by each (a - b·α) mod (f_q, q).
                ModularPoly target_mp(1);
                bool degenerate = false;
                for (const auto& [a, b] : ab_pairs) {
                    std::vector<uint64_t> factor_coeffs(2);
                    int64_t a_mod_s = a % static_cast<int64_t>(q_cand);
                    if (a_mod_s < 0) a_mod_s += static_cast<int64_t>(q_cand);
                    factor_coeffs[0] = static_cast<uint64_t>(a_mod_s);
                    uint64_t b_mod = b % q_cand;
                    factor_coeffs[1] = (q_cand - b_mod) % q_cand;  // -b mod q
                    ModularPoly factor(std::move(factor_coeffs));
                    target_mp = ModularPoly::mul(target_mp, factor, f_q, q_cand);
                }
                if (target_mp.is_zero()) {
                    // S(α) ≡ 0 mod (f_q, q) — character undefined; skip
                    degenerate = true;
                }
                if (degenerate) continue;

                // Apply f'(α)² correction if active (matches compute_product_mod_p
                // post-processing in the main path)
                if (apply_f_prime_correction) {
                    auto fp_q_coeffs = get_f_prime_mod_p(q_cand);
                    if (fp_q_coeffs.empty()) continue;
                    ModularPoly fp_mp(std::move(fp_q_coeffs));
                    if (fp_mp.is_zero()) continue;  // f' ≡ 0 mod q — ramified
                    ModularPoly fp_sq = ModularPoly::mul(fp_mp, fp_mp, f_q, q_cand);
                    target_mp = ModularPoly::mul(target_mp, fp_sq, f_q, q_cand);
                }

                // Store full polynomial target (d coefficients in F_q).
                CharacterPrime cp;
                cp.q = q_cand;
                cp.f_q = std::move(f_q);
                cp.target_q.assign(d, 0);
                for (size_t i = 0; i < d && i <= static_cast<size_t>(target_mp.degree()); ++i) {
                    cp.target_q[i] = target_mp.coeff(i);
                }
                char_primes.push_back(std::move(cp));
            }
        }
        metrics_.character_primes_used = char_primes.size();
        metrics_.character_primes_checked = char_primes_checked;

        // Precompute (m_to_alpha_coeffs) per character prime — actually,
        // we evaluate Y at r_q where Y is in Z[α]/N. So we need current_coeffs
        // (which are CRT-recovered coeffs in [0, M-1]) center-reduced to
        // [-M/2, M/2], then evaluated at r_q mod q. That's the same as
        // current_coeffs % q evaluated at r_q, BUT signs matter (center
        // around 0 before mod q). Implement in lambda below.

        // v20 优化: current_coeffs 在每次 Gray flip 后立即归约到 [0, M-1],
        // verify_current 内省去 %=M 步骤 (~10μs / 系数 / iter)。
        // 65536 iter × d=6 coeffs × 10μs = ~4 sec 节省 per dependency。
        // v22 优化: Y / c / Y2 用 enclosing scope 复用 buffer (mpz_set 重用 mp_d
        // 而非 mpz_init_set 重新 alloc). 65536 iter × d=6 × 1 alloc = ~393K
        // allocs 节省 per dependency.
        Integer Y_buf;
        Integer c_buf;
        Integer Y2_buf;
        auto verify_current = [&]() -> bool {
            // 不变量: current_coeffs[i] ∈ [0, M-1]。
            // 仅需 center 到 [-M/2, M/2] 再 mod N。
            Y_buf = int64_t(0);  // mpz_set_si 复用 buffer
            for (uint32_t i = 0; i < d; ++i) {
                c_buf = current_coeffs[i];  // mpz_set 复用 buffer
                if (c_buf.compare(half_M) > 0) c_buf -= M;
                c_buf %= n;
                if (c_buf.is_negative()) c_buf += n;
                // Y_buf += c_buf * mpow[i] via fused FMA (drops the in-place mul step)
                mpz_addmul(Y_buf.get_mpz(), c_buf.get_mpz(), mpow[i].get_mpz());
                Y_buf %= n;
            }
            if (Y_buf.is_negative()) Y_buf += n;

            // Y2_buf = Y_buf² mod n — mpz_powm_ui combines mul + mod in one op
            mpz_powm_ui(Y2_buf.get_mpz(), Y_buf.get_mpz(), 2, n.get_mpz());

            return Y2_buf.compare(expected_X2) == 0;
        };

        // Character-based fast filter (2026-05-21, polynomial-level form).
        //
        // ── IMPORTANT CORRECTNESS CAVEAT ──
        // The polynomial-level check Y² ≡ T_q in F_q[x]/(f mod q) presumes
        // S(α) = ∏(a-bα) is a perfect square AT THE INTEGER POLYNOMIAL LEVEL
        // (i.e., in Z[α], not just in Z[α]/N). This holds for SYNTHETIC test
        // dependencies built by duplicating pairs, but FAILS for production
        // GNFS dependencies where S(α) is a square only modulo N (the
        // even-exponent matrix kernel + quadratic-character columns make S(α)
        // a square in Z[α]/N but not in Z[α]).
        //
        // Empirical evidence (test_gnfs_e2e under GNFS_FORCE_COUVEIGNES=1
        // GNFS_COUVEIGNES_CHARS=8): filter rejects all 65536 patterns for
        // every dependency from real GNFS, including the patterns that
        // would have passed full Y² ≡ X² mod N verification.
        //
        // The filter therefore remains DISABLED by default
        // (num_characters = 0 → empty char_primes → lambda returns true).
        // When num_characters > 0 the filter is active and the check_below
        // runs. Useful when the caller knows their dependency is a true
        // Z[α] square (synthetic testing, validation harnesses). Real GNFS
        // callers should leave num_characters = 0.
        //
        // Returns true if all character checks pass (candidate may be valid);
        // false if any character check fails. Always returns true when
        // char_primes is empty.
        std::vector<uint64_t> Y_q_coeffs_buf;
        Y_q_coeffs_buf.reserve(d);
        Integer c_buf_chr;
        auto check_characters = [&]() -> bool {
            for (const auto& cp : char_primes) {
                // Reduce current_coeffs mod q.
                Y_q_coeffs_buf.assign(d, 0);
                bool nonzero = false;
                for (uint32_t i = 0; i < d; ++i) {
                    c_buf_chr = current_coeffs[i];
                    if (c_buf_chr.compare(half_M) > 0) c_buf_chr -= M;
                    Y_q_coeffs_buf[i] = static_cast<uint64_t>(
                        mpz_fdiv_ui(c_buf_chr.get_mpz(), cp.q));
                    if (Y_q_coeffs_buf[i] != 0) nonzero = true;
                }
                if (!nonzero) {
                    bool target_zero = true;
                    for (uint64_t c : cp.target_q) {
                        if (c != 0) { target_zero = false; break; }
                    }
                    if (!target_zero) return false;
                    continue;
                }

                // Y_q² mod (f_q, q) — copy buf because ModularPoly takes ownership
                std::vector<uint64_t> Y_q_copy = Y_q_coeffs_buf;
                ModularPoly Y_q_mp{std::move(Y_q_copy)};
                ModularPoly Y_q_sq = ModularPoly::mul(Y_q_mp, Y_q_mp, cp.f_q, cp.q);

                for (size_t i = 0; i < d; ++i) {
                    uint64_t got = (i <= static_cast<size_t>(Y_q_sq.degree())) ? Y_q_sq.coeff(i) : 0;
                    if (got != cp.target_q[i]) {
                        return false;
                    }
                }
            }
            return true;
        };

        auto extract_result = [&]() -> std::vector<Integer> {
            // 同 verify, current_coeffs 已在 [0, M-1]
            // v22: r[i] = current_coeffs[i] (mpz_set into default-init)
            std::vector<Integer> r(d);
            for (uint32_t i = 0; i < d; ++i) {
                r[i] = current_coeffs[i];
                if (r[i].compare(half_M) > 0) r[i] -= M;
                r[i] %= n;
                if (r[i].is_negative()) r[i] += n;
            }
            return r;
        };

        // 初始 base_coeffs 已经 ∈ [0, M-1] (上面 base CRT 后 %=M 归约,
        // 但 center 步骤可能让它 ∈ [-M/2, M/2-1]); v20 需保持 [0, M-1]
        // 不变量,这里 undo center,让其重回 [0, M-1]。
        for (uint32_t i = 0; i < d; ++i) {
            if (current_coeffs[i].is_negative()) current_coeffs[i] += M;
        }

        // Check pattern 0 (all positive)
        metrics_.sign_patterns_tried = 1;
        if (check_characters()) {
            ++metrics_.full_verifications;
            if (verify_current()) {
                metrics_.found_sqrt = true;
                return NumberFieldElement(extract_result());
            }
        } else {
            ++metrics_.character_filter_rejects;
        }

        // Gray code iteration: pattern g = i ^ (i >> 1)
        // Bit that flips: trailing zeros of i gives the position
        uint64_t prev_gray = 0;
        for (uint64_t i = 1; i < max_patterns; ++i) {
            uint64_t gray = i ^ (i >> 1);
            uint64_t changed_bit = prev_gray ^ gray;
            size_t bit_pos = static_cast<size_t>(gnfs::util::ctz64(changed_bit));
            bool new_sign = (gray >> bit_pos) & 1;  // 1 = negative

            // Incremental CRT update + 立即归约到 [0, M-1]
            // two_weights[k][i] ∈ [0, 2M-2], current ∈ [0, M-1]
            // Subtract: result ∈ [-(2M-2), M-1] → 最多 +M 两次
            // Add:      result ∈ [0, 3M-3]      → 最多 -M 两次
            for (uint32_t ci = 0; ci < d; ++ci) {
                if (new_sign) {
                    current_coeffs[ci] -= two_weights[bit_pos][ci];
                    if (current_coeffs[ci].is_negative()) {
                        current_coeffs[ci] += M;
                        if (current_coeffs[ci].is_negative()) current_coeffs[ci] += M;
                    }
                } else {
                    current_coeffs[ci] += two_weights[bit_pos][ci];
                    if (current_coeffs[ci].compare(M) >= 0) {
                        current_coeffs[ci] -= M;
                        if (current_coeffs[ci].compare(M) >= 0) current_coeffs[ci] -= M;
                    }
                }
            }

            prev_gray = gray;
            ++metrics_.sign_patterns_tried;

            // Cheap character filter before expensive Y² ≡ X² mod N check.
            // When num_characters = 0 (default), check_characters() returns
            // true unconditionally and behavior is identical to legacy code.
            if (!check_characters()) {
                ++metrics_.character_filter_rejects;
                continue;
            }
            ++metrics_.full_verifications;
            if (verify_current()) {
                metrics_.found_sqrt = true;
                return NumberFieldElement(extract_result());
            }
        }

        return std::nullopt;  // No valid sign pattern found
    }

    /// Compute square root directly from NumberFieldElement
    /// Uses Gray code sign enumeration (same as compute()) instead of
    /// the broken eval-at-1 heuristic which used different thresholds p/2
    /// per prime, producing inconsistent signs → CRT reconstruction garbage.
    [[nodiscard]] std::optional<NumberFieldElement> compute_from_element(
            const NumberFieldElement& elem,
            const NumberField& nf) const {

        metrics_.reset();
        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // mpz_fdiv_ui returns floor-div remainder ∈ [0, p-1] regardless of sign (zero alloc)
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                f[i] = static_cast<uint64_t>(mpz_fdiv_ui(nf.coeff(i).get_mpz(), p));
            }
            return f;
        };

        auto elem_to_mod_p = [&elem, d](uint64_t p) -> ModularPoly {
            std::vector<uint64_t> coeffs(d);
            for (uint32_t i = 0; i < d && i <= elem.degree(); ++i) {
                coeffs[i] = static_cast<uint64_t>(mpz_fdiv_ui(elem.coeff(i).get_mpz(), p));
            }
            return ModularPoly(std::move(coeffs));
        };

        // Collect suitable primes — store raw sqrt coefficients without sign normalization
        // Reserve config_.num_primes (the loop exit criterion).
        std::vector<uint64_t> primes;
        primes.reserve(config_.num_primes);
        std::vector<std::vector<uint64_t>> sqrt_coeffs;
        sqrt_coeffs.reserve(config_.num_primes);

        uint64_t p = config_.prime_start;
        size_t primes_checked = 0;
        while (primes.size() < config_.num_primes && primes_checked < config_.max_prime_checks) {
            p = next_prime(p);
            primes_checked++;

            // Skip primes that divide N — direct GMP divisibility (zero alloc)
            if (mpz_divisible_ui_p(n.get_mpz(), p)) continue;

            auto f_mod_p = get_f_mod_p(p);
            if (f_mod_p.back() == 0) continue;

            if (!ModularPoly::is_irreducible(f_mod_p, p)) continue;

            auto elem_mod_p = elem_to_mod_p(p);
            if (elem_mod_p.is_zero()) continue;

            if (!ModularPoly::is_square(elem_mod_p, f_mod_p, p)) continue;

            auto sqrt_p = ModularPoly::sqrt_tonelli_shanks(elem_mod_p, f_mod_p, p);
            if (sqrt_p.is_zero() && !elem_mod_p.is_zero()) continue;

            // Verify: sqrt^2 == elem mod p
            auto sq = ModularPoly::mul(sqrt_p, sqrt_p, f_mod_p, p);
            bool valid = true;
            for (size_t i = 0; i <= std::max(static_cast<size_t>(sq.degree()),
                                              static_cast<size_t>(elem_mod_p.degree())); ++i) {
                if (sq.coeff(i) != elem_mod_p.coeff(i)) { valid = false; break; }
            }
            if (!valid) continue;

            // Store raw coefficients — sign resolved via Gray code below
            primes.push_back(p);
            std::vector<uint64_t> coeffs(d, 0);
            for (size_t i = 0; i < d && i <= static_cast<size_t>(sqrt_p.degree()); ++i) {
                coeffs[i] = sqrt_p.coeff(i);
            }
            sqrt_coeffs.push_back(std::move(coeffs));
        }

        if (primes.size() < 2) return std::nullopt;

        // CRT modulus (Couveignes primes ≤ uint32 max)
        Integer M(1);
        for (uint64_t prime : primes) M *= static_cast<int64_t>(prime);  // mpz_mul_si direct

        // Precompute CRT weights: weight[j][i] = c_ij * M_j * M_j_inv mod M
        // v22: M_j / M_j_mod_pj 复用 + p_j 用 int64_t 直接 (primes ≤ uint32 max)
        std::vector<std::vector<Integer>> weights(primes.size());
        Integer M_j_b, M_j_mod_pj_b;
        Integer w;  // hoist — reused per (j, i) iter
        for (size_t j = 0; j < primes.size(); ++j) {
            uint64_t p_j = primes[j];
            M_j_b = M;
            M_j_b /= int64_t(p_j);
            M_j_mod_pj_b = M_j_b;
            M_j_mod_pj_b %= int64_t(p_j);
            uint64_t M_j_inv = mod_inverse_u64(M_j_mod_pj_b.to_uint64(), p_j);

            weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                w = uint64_t(sqrt_coeffs[j][i]);  // mpz_set_ui (no init)
                w *= M_j_b;
                w *= static_cast<int64_t>(M_j_inv);
                w %= M;
                weights[j][i] = w;  // mpz_set into resized slot (skip tmp clone)
            }
        }

        // Base CRT (all signs = +)
        std::vector<Integer> base_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            Integer coeff_i;  // default ctor = 0
            for (size_t j = 0; j < primes.size(); ++j) {
                coeff_i += weights[j][i];
            }
            coeff_i %= M;
            base_coeffs[i] = std::move(coeff_i);
        }

        // v22: half_M 直接 assign
        Integer half_M;
        half_M = M;
        mpz_tdiv_q_2exp(half_M.get_mpz(), half_M.get_mpz(), 1);

        // Center around 0
        for (auto& c : base_coeffs) {
            if (c.compare(half_M) > 0) c -= M;
        }

        // Precompute 2 * weight for incremental Gray code updates
        // v22: two_weights[j][i] = weights[j][i] (mpz_set)
        std::vector<std::vector<Integer>> two_weights(primes.size());
        for (size_t j = 0; j < primes.size(); ++j) {
            two_weights[j].resize(d);
            for (uint32_t i = 0; i < d; ++i) {
                // mpz_mul_2exp = bit shift, fastest power-of-2 multiply
                mpz_mul_2exp(two_weights[j][i].get_mpz(), weights[j][i].get_mpz(), 1);
            }
        }

        // Expected value: elem(m) mod N — candidate Y must satisfy Y² ≡ elem(m) mod N
        Integer expected_X2 = nf.evaluate_at_m_mod_n(elem);

        // v22: current_coeffs[i] = base_coeffs[i] (mpz_set into default-init)
        std::vector<Integer> current_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            current_coeffs[i] = base_coeffs[i];
            // v21: 保 [0, M-1] 不变量(同 compute() 路径,见 v20 注释)
            if (current_coeffs[i].is_negative()) current_coeffs[i] += M;
        }

        // Same cap as compute() — see comment there. Throw rather than silently
        // truncate the search.
        if (primes.size() > 16) {
            throw std::logic_error(
                "Couveignes::compute_from_element: >16 primes not supported in 65536-pattern search");
        }
        size_t num_to_search = primes.size();
        uint64_t max_patterns = 1ULL << num_to_search;

        // v21: 同步 compute() 的 v17 (mpow 缓存) + v20 ([0,M-1] 不变量) 优化
        // 旧实现 evaluate_at_m_mod_n(NumberFieldElement(cand)) 每 iter 构造
        // NumberFieldElement (d 次 clone + 移动) 再 Horner,~50μs/iter 开销。
        // 内联 Horner + mpow 缓存,省 NumberFieldElement 构造,~5μs/iter。
        // v22: mpow[j] = mpow[j-1] (mpz_set)
        std::vector<Integer> mpow(d);
        mpow[0] = int64_t(1);  // mpz_set_si direct
        for (uint32_t j = 1; j < d; ++j) {
            mpow[j] = mpow[j-1];
            mpow[j] *= nf.m();
            mpow[j] %= n;
        }

        // v22: Y / c / Y2 复用 enclosing scope buffer 减少 ~393K alloc/dep
        // (see compute() 同等 path docs)
        Integer Y_buf;
        Integer c_buf;
        Integer Y2_buf;
        auto verify_current = [&]() -> bool {
            // 不变量: current_coeffs[i] ∈ [0, M-1]
            Y_buf = int64_t(0);  // mpz_set_si direct
            for (uint32_t i = 0; i < d; ++i) {
                c_buf = current_coeffs[i];  // mpz_set 复用 buffer
                if (c_buf.compare(half_M) > 0) c_buf -= M;
                c_buf %= n;
                if (c_buf.is_negative()) c_buf += n;
                // Y_buf += c_buf * mpow[i] via fused FMA
                mpz_addmul(Y_buf.get_mpz(), c_buf.get_mpz(), mpow[i].get_mpz());
                Y_buf %= n;
            }
            if (Y_buf.is_negative()) Y_buf += n;

            // Y2_buf = Y_buf² mod n — mpz_powm_ui combines mul + mod in one op
            mpz_powm_ui(Y2_buf.get_mpz(), Y_buf.get_mpz(), 2, n.get_mpz());
            return Y2_buf.compare(expected_X2) == 0;
        };

        auto extract_result = [&]() -> std::vector<Integer> {
            // v22: r[i] = current_coeffs[i] (mpz_set into default-init)
            std::vector<Integer> r(d);
            for (uint32_t i = 0; i < d; ++i) {
                r[i] = current_coeffs[i];
                if (r[i].compare(half_M) > 0) r[i] -= M;
                r[i] %= n;
                if (r[i].is_negative()) r[i] += n;
            }
            return r;
        };

        // Populate basic metrics for compute_from_element (no per-skip breakdown
        // because this path's prime selection has tighter constraints).
        metrics_.primes_checked = primes_checked;
        metrics_.primes_used = primes.size();

        // Check pattern 0 (all positive)
        metrics_.sign_patterns_tried = 1;
        metrics_.full_verifications = 1;
        if (verify_current()) {
            metrics_.found_sqrt = true;
            return NumberFieldElement(extract_result());
        }

        // Gray code enumeration over sign combinations,同样 maintain [0, M-1]
        uint64_t prev_gray = 0;
        for (uint64_t i = 1; i < max_patterns; ++i) {
            uint64_t gray = i ^ (i >> 1);
            uint64_t changed_bit = prev_gray ^ gray;
            size_t bit_pos = static_cast<size_t>(gnfs::util::ctz64(changed_bit));
            bool new_sign = (gray >> bit_pos) & 1;

            for (uint32_t ci = 0; ci < d; ++ci) {
                if (new_sign) {
                    current_coeffs[ci] -= two_weights[bit_pos][ci];
                    if (current_coeffs[ci].is_negative()) {
                        current_coeffs[ci] += M;
                        if (current_coeffs[ci].is_negative()) current_coeffs[ci] += M;
                    }
                } else {
                    current_coeffs[ci] += two_weights[bit_pos][ci];
                    if (current_coeffs[ci].compare(M) >= 0) {
                        current_coeffs[ci] -= M;
                        if (current_coeffs[ci].compare(M) >= 0) current_coeffs[ci] -= M;
                    }
                }
            }

            prev_gray = gray;
            ++metrics_.sign_patterns_tried;
            ++metrics_.full_verifications;
            if (verify_current()) {
                metrics_.found_sqrt = true;
                return NumberFieldElement(extract_result());
            }
        }

        return std::nullopt;
    }

private:
    Config config_;
    mutable Metrics metrics_;  // Updated by compute() / compute_from_element()

    /// Compute product of (a_i - b_i * x) mod f(x) mod p (GNFS a - b·α convention)
    ///
    /// ── f'(α)² 修正 (v18, Thomé 2008 论文 §1) ───────────────────────────────
    /// δ = ∏(a-bα) 的 sqrt 在 O_K 一定存在(line-alg 保证),但可能不在 Z[α]。
    /// Thomé 论文 page 1 首段定义: T(α)² = f'(α)²·S(α) — 乘 f'(α)² 后 sqrt
    /// 落入 Z[α]。
    ///
    /// 现 compute() 的主循环中,在 compute_product_mod_p 返回后立即乘
    /// f'(x)² mod f(x) mod p(见 compute() 内 `f_prime_sq` 块),Couveignes
    /// 实际对 (f'(α)² · S(α)) mod p 取 sqrt。CRT 后得 T(α) ≡ f'(α)·√S(α);
    /// caller (algebraic_sqrt.hpp::compute_couveignes) 评估 T(m) 后乘
    /// inv(f'(m)) mod N 还原 √S(m) mod N。
    ///
    /// compute_product_mod_p 自身只算原始 ∏(a-bα) mod p,不含 f'(α)² 修正。
    /// ─────────────────────────────────────────────────────────────────────────
    [[nodiscard]] ModularPoly compute_product_mod_p(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<uint64_t>& f,
            uint64_t p) const {

        ModularPoly product(1);

        for (const auto& [a, b] : ab_pairs) {
            // Create (a - b*x) mod p (GNFS convention)
            std::vector<uint64_t> coeffs(2);

            int64_t a_mod = a % static_cast<int64_t>(p);
            if (a_mod < 0) a_mod += static_cast<int64_t>(p);
            coeffs[0] = static_cast<uint64_t>(a_mod);

            // Coefficient of x is -b mod p
            uint64_t b_mod = b % p;
            coeffs[1] = (p - b_mod) % p;

            ModularPoly factor(std::move(coeffs));

            // Multiply into product
            product = ModularPoly::mul(product, factor, f, p);
        }

        return product;
    }

    /// Delegate next_prime / is_prime_u64 / pow_mod_u64 / mul_mod_u64 to shared util.
    [[nodiscard]] static uint64_t next_prime(uint64_t n) {
        return gnfs::util::next_prime_u64(n);
    }
    [[nodiscard]] static bool is_prime_u64(uint64_t n) {
        return gnfs::util::is_prime_u64(n);
    }
    [[nodiscard]] static uint64_t pow_mod_u64(uint64_t base, uint64_t exp, uint64_t mod) {
        return gnfs::util::pow_mod_u64(base, exp, mod);
    }
    [[nodiscard]] static uint64_t mul_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
        return gnfs::util::mul_mod_u64(a, b, mod);
    }

    /// Legendre symbol (a / p) for odd prime p. Returns +1, -1, or 0.
    /// Used by character verification. Cost: O(log(a) + log(p)) via Euler's
    /// criterion when q is small enough for direct pow_mod. For p < 2^32,
    /// pow_mod is two 64-bit multiplications per bit of (p-1)/2.
    [[nodiscard]] static int legendre_symbol(uint64_t a, uint64_t p) {
        if (p == 2) return (a & 1) ? 1 : 0;
        uint64_t a_mod = a % p;
        if (a_mod == 0) return 0;
        // Euler's criterion: a^((p-1)/2) mod p
        uint64_t r = pow_mod_u64(a_mod, (p - 1) / 2, p);
        if (r == 1) return 1;
        if (r == p - 1) return -1;
        // Should not happen for prime p
        return 0;
    }

    /// Evaluate a polynomial p(x) at integer x = r modulo q via Horner.
    /// Used to compute Y(α) mod q where we choose r = root of f mod q,
    /// reducing the character check to a single Legendre symbol of an integer.
    /// coeffs are uint64 (already reduced mod q).
    /// Cost: O(deg) modular multiplications.
    [[nodiscard]] static uint64_t eval_poly_at_root_mod_q(
            const std::vector<uint64_t>& coeffs,
            uint64_t r,
            uint64_t q) {
        if (coeffs.empty()) return 0;
        // Horner from highest-degree coefficient down
        uint64_t acc = 0;
        for (size_t i = coeffs.size(); i > 0; --i) {
            // acc = acc * r + coeffs[i-1], all mod q
            acc = mul_mod_u64(acc, r, q);
            uint64_t c = coeffs[i - 1] % q;
            acc = (acc + c) % q;
        }
        return acc;
    }

    /// Find the first root r of f mod q, or return uint64_t max if none.
    /// Used to select character primes (we need at least one degree-1 prime
    /// ideal above q for the character to be evaluable).
    /// For small q (< 100k), brute-force search is faster than Cantor-Zassenhaus.
    [[nodiscard]] static uint64_t find_root_mod_q(
            const std::vector<uint64_t>& f_mod_q,
            uint64_t q) {
        // Build f mod q, evaluate at every x ∈ [0, q-1]
        for (uint64_t x = 0; x < q; ++x) {
            uint64_t val = 0;
            uint64_t x_power = 1;
            for (size_t i = 0; i < f_mod_q.size(); ++i) {
                val = (val + mul_mod_u64(f_mod_q[i], x_power, q)) % q;
                x_power = mul_mod_u64(x_power, x, q);
            }
            if (val == 0) return x;
        }
        return ~uint64_t(0);  // No root
    }

    /// Modular inverse
    [[nodiscard]] static uint64_t mod_inverse_u64(uint64_t a, uint64_t p) {
        int64_t t = 0, new_t = 1;
        int64_t r = static_cast<int64_t>(p), new_r = static_cast<int64_t>(a);

        while (new_r != 0) {
            int64_t quotient = r / new_r;

            int64_t temp_t = new_t;
            new_t = t - quotient * new_t;
            t = temp_t;

            int64_t temp_r = new_r;
            new_r = r - quotient * new_r;
            r = temp_r;
        }

        if (t < 0) {
            t += static_cast<int64_t>(p);
        }

        return static_cast<uint64_t>(t);
    }
};

} // namespace gnfs::sqrt
