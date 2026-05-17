#pragma once

#include "number_field.hpp"
#include "modular_poly.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
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

    CouveignesSqrt() : config_() {}

    explicit CouveignesSqrt(const Config& config)
        : config_(config) {}

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

        if (ab_pairs.empty()) {
            return std::nullopt;
        }

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Get polynomial coefficients mod p
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            const Integer p_int(p);  // hoist out of loop
            for (uint32_t i = 0; i <= d; ++i) {
                Integer coeff = nf.coeff(i).clone();
                coeff %= p_int;
                if (coeff.is_negative()) {
                    coeff += p_int;
                }
                f[i] = coeff.to_uint64();
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

        uint64_t p = config_.prime_start;
        while (primes.size() < config_.num_primes && primes_checked < config_.max_prime_checks) {
            p = next_prime(p);
            primes_checked++;

            // Skip primes that divide N
            Integer n_mod_p = n.clone();
            n_mod_p %= Integer(p);
            if (n_mod_p.is_zero()) {
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
                    // f' ≡ 0 mod p — 极少见(p | gcd(所有 i·f[i])),跳过此素数
                    primes_zero_product++;
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

        // Suppress unused variable warnings
        (void)primes_checked;
        (void)primes_dividing_n;
        (void)primes_bad_leading;
        (void)primes_reducible;
        (void)primes_zero_product;
        (void)primes_no_sqrt;

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
                weights[j][i] = w.clone();
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
                two_weights[j][i] = weights[j][i];
                two_weights[j][i] *= int64_t(2);  // mpz_mul_si direct
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
        // v22: term/bm 复用 across ab_pairs (hot loop, 10K+ iters per dep)
        Integer expected_X2(1);
        Integer term_buf, bm;
        for (const auto& [a, b] : ab_pairs) {
            term_buf = a;  // mpz_set_si direct
            bm = nf.m();
            bm *= static_cast<int64_t>(b);  // mpz_mul_si direct (b ≤ sieve bound, fits)
            term_buf -= bm;
            term_buf %= n;
            if (term_buf.is_negative()) term_buf += n;
            expected_X2 *= term_buf;
            expected_X2 %= n;
        }
        // 乘 f'(m)² mod N (仅 apply_f_prime_correction=true 时)
        if (apply_f_prime_correction) {
            // f'(m) = Σ_{i=1}^d i · f[i] · m^(i-1)
            // v22: term/f_prime_m_sq 直接 assign
            const Integer& m_val = nf.m();
            Integer f_prime_m;  // default ctor = 0
            Integer term_h;
            for (int i = static_cast<int>(d); i >= 1; --i) {
                f_prime_m *= m_val;
                term_h = nf.coeff(static_cast<uint32_t>(i));
                term_h *= static_cast<int64_t>(i);  // mpz_mul_si direct
                f_prime_m += term_h;
                f_prime_m %= n;
            }
            if (f_prime_m.is_negative()) f_prime_m += n;
            Integer f_prime_m_sq;
            f_prime_m_sq = f_prime_m;
            f_prime_m_sq *= f_prime_m;
            f_prime_m_sq %= n;
            expected_X2 *= f_prime_m_sq;
            expected_X2 %= n;
        }

        // m^j mod N 缓存,Gray code 内每次 verify 不再重算 (v22: mpz_set)
        std::vector<Integer> mpow(d);
        mpow[0] = int64_t(1);  // mpz_set_si direct
        for (uint32_t j = 1; j < d; ++j) {
            mpow[j] = mpow[j-1];
            mpow[j] *= nf.m();
            mpow[j] %= n;
        }

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
                c_buf *= mpow[i];
                Y_buf += c_buf;
                Y_buf %= n;
            }
            if (Y_buf.is_negative()) Y_buf += n;

            Y2_buf = Y_buf;  // mpz_set 复用 buffer
            Y2_buf *= Y_buf;
            Y2_buf %= n;

            return Y2_buf.compare(expected_X2) == 0;
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
        if (verify_current()) {
            return NumberFieldElement(extract_result());
        }

        // Gray code iteration: pattern g = i ^ (i >> 1)
        // Bit that flips: trailing zeros of i gives the position
        uint64_t prev_gray = 0;
        for (uint64_t i = 1; i < max_patterns; ++i) {
            uint64_t gray = i ^ (i >> 1);
            uint64_t changed_bit = prev_gray ^ gray;
            size_t bit_pos = __builtin_ctzll(changed_bit);
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

            if (verify_current()) {
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

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // v22: 内部 buffer 复用 (mpz_set 而非 mpz_init_set)
        auto get_f_mod_p = [&nf, d](uint64_t p) -> std::vector<uint64_t> {
            std::vector<uint64_t> f(d + 1);
            Integer coeff;
            const Integer p_int(p);
            for (uint32_t i = 0; i <= d; ++i) {
                coeff = nf.coeff(i);
                coeff %= p_int;
                if (coeff.is_negative()) coeff += p_int;
                f[i] = coeff.to_uint64();
            }
            return f;
        };

        auto elem_to_mod_p = [&elem, d](uint64_t p) -> ModularPoly {
            std::vector<uint64_t> coeffs(d);
            Integer c;
            const Integer p_int(p);
            for (uint32_t i = 0; i < d && i <= elem.degree(); ++i) {
                c = elem.coeff(i);
                c %= p_int;
                if (c.is_negative()) c += p_int;
                coeffs[i] = c.to_uint64();
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

            // v22: n_mod_p 直接 assign (mpz_set)
            Integer n_mod_p;
            n_mod_p = n;
            n_mod_p %= Integer(p);
            if (n_mod_p.is_zero()) continue;

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
                weights[j][i] = w.clone();
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
                two_weights[j][i] = weights[j][i];
                two_weights[j][i] *= int64_t(2);  // mpz_mul_si direct
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
                c_buf *= mpow[i];
                Y_buf += c_buf;
                Y_buf %= n;
            }
            if (Y_buf.is_negative()) Y_buf += n;

            Y2_buf = Y_buf;
            Y2_buf *= Y_buf;
            Y2_buf %= n;
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

        // Check pattern 0 (all positive)
        if (verify_current()) {
            return NumberFieldElement(extract_result());
        }

        // Gray code enumeration over sign combinations,同样 maintain [0, M-1]
        uint64_t prev_gray = 0;
        for (uint64_t i = 1; i < max_patterns; ++i) {
            uint64_t gray = i ^ (i >> 1);
            uint64_t changed_bit = prev_gray ^ gray;
            size_t bit_pos = __builtin_ctzll(changed_bit);
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

            if (verify_current()) {
                return NumberFieldElement(extract_result());
            }
        }

        return std::nullopt;
    }

private:
    Config config_;

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
