#pragma once

#include "number_field.hpp"
#include "modular_poly.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <vector>
#include <optional>
#include <iostream>
#include <thread>
#include <chrono>

// ─── BACKLOG P3 DEBT — Hensel verbose 编译期裁剪 ──────────────────────────
// 大部分诊断 std::cerr 路径运行时已被 config_.verbose 门控,默认 verbose=false
// 时分支不进入。但编译期仍生成代码占用 ~kB binary 体积。定义
// GNFS_HENSEL_NO_VERBOSE 即可在 release 构建中完全裁剪这些块。
// 默认行为不变 (与原 if(config_.verbose) 等价)。
#ifndef GNFS_HENSEL_NO_VERBOSE
  #define HENSEL_VERBOSE(stmt) do { if (config_.verbose) { stmt; } } while (0)
#else
  #define HENSEL_VERBOSE(stmt) ((void)0)
#endif

namespace gnfs::sqrt {

using core::Integer;
using core::PolynomialContext;

/// Hensel lifting algebraic square root
///
/// Computes algebraic sqrt by:
/// 1. Finding sqrt mod (f, p) for a single inert prime p
/// 2. Hensel lifting to mod (f, p^{2^k}) until precision suffices
/// 3. Evaluating at m mod N to get the integer result
///
/// This avoids the CRT sign determination problem entirely.
class HenselSqrt {
public:
    struct Config {
        uint64_t prime_start = 1000;   // Starting prime search
        size_t extra_precision = 200;  // Extra bits of precision beyond estimate
        uint64_t cached_inert_prime = 0; // Pre-found inert prime (0 = auto-find)
        bool verbose = false;
    };

    HenselSqrt() = default;
    explicit HenselSqrt(const Config& config) : config_(config) {}

    /// Get the inert prime found/used during the last compute() call
    [[nodiscard]] uint64_t last_inert_prime() const noexcept { return last_inert_prime_; }

    /// True if CRT searched all 2^15 sign combos without finding a match.
    /// When true, the dependency is almost certainly invalid — skip fallback methods.
    [[nodiscard]] bool was_crt_sign_exhausted() const noexcept { return crt_sign_exhausted_; }

    /// Compute algebraic square root value (mod N)
    ///
    /// Uses multi-prime CRT (Nguyen 2004) for large inputs:
    /// compute sqrt(P·f'(α)²) independently mod many small primes,
    /// then reconstruct via CRT. Falls back to Hensel lifting for small inputs
    /// or if CRT fails.
    [[nodiscard]] std::optional<Integer> compute(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {

        if (ab_pairs.empty()) return Integer(int64_t(1));
        auto t0_compute = std::chrono::steady_clock::now();

        const Integer& n = nf.n();

        // Estimate required precision for the sqrt coefficients
        double target_bits = estimate_target_bits(ab_pairs, nf);

        // Pre-compute verification product P(m) = ∏(a_i - b_i*m) mod N
        Integer product_at_m = compute_product_at_m(ab_pairs, nf);

        // Compute f'(m) mod N and its inverse
        Integer f_prime_m = evaluate_f_derivative_at_m(nf);
        Integer f_prime_m_inv;
        {
            int ok = mpz_invert(f_prime_m_inv.get_mpz(), f_prime_m.get_mpz(), n.get_mpz());
            if (!ok) {
                if (config_.verbose) {
                    std::cerr << "[Hensel] f'(m) not invertible mod N\n";
                }
                return std::nullopt;
            }
        }

        // Nguyen hybrid: K small primes + Hensel lift each + CRT + small sign search
        // For small inputs (<100 factors), single-prime Hensel is fast enough.
        // Retry Nguyen with doubled precision before falling back to slow single-prime.
        crt_sign_exhausted_ = false;
        if (ab_pairs.size() >= 100) {
            double nguyen_target = target_bits;
            for (int nguyen_attempt = 0; nguyen_attempt < 3; ++nguyen_attempt) {
                auto result = compute_nguyen_hybrid(
                    ab_pairs, nf, nguyen_target, product_at_m, f_prime_m, f_prime_m_inv);
                if (result) {
                    if (config_.verbose) {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0_compute).count();
                        std::cerr << "[Hensel] compute() total: " << ms << "ms\n";
                    }
                    return result;
                }
                if (crt_sign_exhausted_) {
                    // In F_{p^d} (a field for inert p), there are exactly 2 square
                    // roots ±S. The 2^(K-1) sign search covers all combos. If ALL
                    // fail, the product is NOT a perfect square — dep is invalid.
                    // This holds regardless of prime replacement.
                    if (config_.verbose) {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0_compute).count();
                        std::cerr << "[Nguyen] CRT exhausted (" << ms
                                  << "ms) — dep invalid\n";
                    }
                    return std::nullopt;
                }
                // Double precision for next Nguyen attempt
                nguyen_target *= 2.0;
                if (config_.verbose) {
                    std::cerr << "[Nguyen] Attempt " << nguyen_attempt
                              << " failed, retrying with target="
                              << static_cast<size_t>(nguyen_target) << " bits\n";
                }
            }
            if (config_.verbose) {
                std::cerr << "[Nguyen] All Nguyen attempts exhausted, "
                             "falling back to single-prime Hensel\n";
            }
        }

        // Fallback: classic single-prime Hensel lifting (slow for large inputs)
        return compute_hensel_lifting(
            ab_pairs, nf, target_bits, product_at_m, f_prime_m, f_prime_m_inv);
    }

private:
    Config config_;
    mutable uint64_t last_inert_prime_ = 0;
    mutable bool crt_sign_exhausted_ = false;  // true if CRT searched all combos

    /// Estimate target bits for sqrt coefficient recovery.
    ///
    /// For sqrt S of product γ in Z[α]/(f), the coefficient bound comes from:
    ///   |sⱼ| ≤ ||V⁻¹||∞ · max_k |σ_k(S)| · |f'(α_k)|
    /// where σ_k are the d complex embeddings and V is the Vandermonde matrix.
    /// The LEADING term is log_bound/2 (from the worst single embedding),
    /// with O(d·log R) correction from the Vandermonde inverse and f'(α).
    [[nodiscard]] double estimate_target_bits(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) const {
        uint32_t d = nf.degree();
        double max_root = std::abs(nf.m().to_double());
        {
            double c_d_abs = std::abs(nf.coeff(d).to_double());
            if (c_d_abs > 0) {
                for (uint32_t i = 0; i < d; ++i) {
                    double ratio = std::abs(nf.coeff(i).to_double()) / c_d_abs;
                    max_root = std::max(max_root, 1.0 + ratio);
                }
            }
        }
        double log_bound = 0;
        for (const auto& [a, b] : ab_pairs) {
            double val = std::abs(static_cast<double>(a)) +
                         static_cast<double>(b) * max_root;
            log_bound += std::log2(std::max(val, 1.0));
        }
        // f'(α) bound: |f'(α_k)| ≤ d · R^{d-1}
        double log_f_prime_bound = std::log2(static_cast<double>(d));
        if (d > 1) log_f_prime_bound += static_cast<double>(d) * std::log2(max_root + 1.0);

        // Leading term: log_bound/2 (sqrt of worst-case single embedding)
        // Correction: log_f_prime_bound (f'(α)² trick), log₂(d) (Vandermonde)
        // Adaptive safety margin:大类群/大 R 下 extra_precision=200 常量
        // 可能偏低 ~50 bits 导致 center 步骤错位 → -Y 兜底失败 50% 概率。
        // 改为 max(extra_precision, 0.05 * log_bound) 让安全余量随关系数
        // 量级 scaling。Stage1 估算 log_bound ≈ 10000 时 safety = 500,
        // 远超原 200。小问题(<4000 bits)不受影响。
        double adaptive_safety = std::max(
            static_cast<double>(config_.extra_precision),
            log_bound * 0.05);
        return log_bound / 2.0 + log_f_prime_bound
               + std::log2(static_cast<double>(d))
               + adaptive_safety;
    }

    /// Compute ∏(a_i - b_i*m) mod N
    [[nodiscard]] static Integer compute_product_at_m(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf) {
        const Integer& n = nf.n();
        Integer product(int64_t(1));
        for (const auto& [a, b] : ab_pairs) {
            Integer factor(a);
            Integer bm = nf.m().clone();
            bm *= Integer(b);
            factor -= bm;
            factor %= n;
            if (factor.is_negative()) factor += n;
            product *= factor;
            product %= n;
        }
        return product;
    }

    /// Verify Y and return it or -Y if verification passes
    [[nodiscard]] static std::optional<Integer> verify_and_return(
            const Integer& Y, const Integer& product_at_m, const Integer& n) {
        Integer Y2 = Y.clone();
        Y2 *= Y;
        Y2 %= n;
        if (Y2.is_negative()) Y2 += n;

        Integer pm_pos = product_at_m.clone();
        if (pm_pos.is_negative()) pm_pos += n;

        if (Y2.compare(pm_pos) == 0) return Y.clone();

        // Check -Y
        Integer neg_Y = n.clone();
        neg_Y -= Y;
        Integer neg_Y2 = neg_Y.clone();
        neg_Y2 *= neg_Y;
        neg_Y2 %= n;
        if (neg_Y2.is_negative()) neg_Y2 += n;

        if (neg_Y2.compare(pm_pos) == 0) return neg_Y;

        return std::nullopt;
    }

    // ========================================================================
    // Nguyen Hybrid: K small primes + Hensel lift + CRT + small sign search
    // ========================================================================

    /// Find multiple small inert primes for Nguyen hybrid
    [[nodiscard]] std::vector<uint64_t> find_inert_primes(
            const NumberField& nf, size_t count) const {
        std::vector<uint64_t> primes;
        primes.reserve(count);
        uint64_t p = config_.prime_start;
        for (size_t att = 0; primes.size() < count && att < 100000; ++att) {
            p = next_prime(p);
            auto f_mod = get_f_mod_p(nf, p);
            if (f_mod.back() == 0) continue;
            if (ModularPoly::is_irreducible(f_mod, p)) {
                primes.push_back(p);
            }
        }
        return primes;
    }

    /// Core Hensel lift for one prime: given sqrt mod p, lift to mod p^{2^num_lifts}
    /// Returns d Integer coefficients of the lifted sqrt, plus the final modulus.
    struct LiftResult {
        std::vector<Integer> coeffs;  // sqrt coefficients mod modulus
        Integer modulus;               // p^{2^num_lifts}
        bool ok = false;
    };

    [[nodiscard]] LiftResult hensel_lift_single_prime(
            uint64_t p,
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            size_t num_lifts,
            size_t max_threads = 0) const {

        uint32_t d = nf.degree();
        LiftResult result;

        // Compute sqrt mod (f, p) using fast ModularPoly
        auto f_mod_p = get_f_mod_p(nf, p);
        ModularPoly product_mod_p(1);
        for (const auto& [a, b] : ab_pairs) {
            std::vector<uint64_t> cs(2);
            int64_t am = a % static_cast<int64_t>(p);
            if (am < 0) am += static_cast<int64_t>(p);
            cs[0] = static_cast<uint64_t>(am);
            cs[1] = (p - (b % p)) % p;
            product_mod_p = ModularPoly::mul(
                product_mod_p, ModularPoly(std::move(cs)), f_mod_p, p);
        }
        if (product_mod_p.is_zero()) {
            HENSEL_VERBOSE(std::cerr << "[Hensel-lift] p=" << p << " product is zero\n");
            return result;
        }

        // Multiply by f'(α)² mod (f, p)
        auto f_prime_mod_p_vec = compute_f_derivative_mod_p(nf, p);
        auto f_prime_poly = ModularPoly(f_prime_mod_p_vec);
        auto fp2 = ModularPoly::mul(f_prime_poly, f_prime_poly, f_mod_p, p);
        product_mod_p = ModularPoly::mul(product_mod_p, fp2, f_mod_p, p);

        if (!ModularPoly::is_square(product_mod_p, f_mod_p, p)) {
            HENSEL_VERBOSE(std::cerr << "[Hensel-lift] p=" << p << " product*f'^2 not square\n");
            return result;
        }
        auto sqrt_mp = ModularPoly::sqrt_tonelli_shanks(product_mod_p, f_mod_p, p);
        if (sqrt_mp.is_zero() && !product_mod_p.is_zero()) {
            HENSEL_VERBOSE(std::cerr << "[Hensel-lift] p=" << p << " Tonelli-Shanks returned zero\n");
            return result;
        }

        // Convert to Integer polynomial S
        std::vector<Integer> S(d);
        for (uint32_t i = 0; i < d; ++i) {
            S[i] = Integer(static_cast<int64_t>(
                (i <= static_cast<uint32_t>(sqrt_mp.degree())) ? sqrt_mp.coeff(i) : 0));
        }

        if (num_lifts == 0) {
            result.coeffs = std::move(S);
            result.modulus = Integer(static_cast<int64_t>(p));
            result.ok = true;
            return result;
        }

        // Get f polynomial
        std::vector<Integer> f_int(d + 1);
        for (uint32_t i = 0; i <= d; ++i) f_int[i] = nf.coeff(i).clone();

        Integer modulus(static_cast<int64_t>(p));

        // Compute T₀ = (2·S₀)^{-1} mod (f, p) using Fermat
        std::vector<Integer> T(d);
        {
            std::vector<uint64_t> two_s_mod(d);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t si = S[i].to_uint64();
                two_s_mod[i] = (2 * si) % p;
            }
            Integer q_minus_2;
            mpz_ui_pow_ui(q_minus_2.get_mpz(), p, d);
            q_minus_2 -= Integer(int64_t(2));
            auto inv_mp = ModularPoly::power(
                ModularPoly(two_s_mod), q_minus_2, f_mod_p, p);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t cv = (i <= static_cast<uint32_t>(inv_mp.degree()))
                              ? inv_mp.coeff(i) : 0;
                T[i] = Integer(cv);
            }
        }

        // Pre-compute product at final precision (once)
        Integer final_mod(static_cast<int64_t>(p));
        for (size_t i = 0; i < num_lifts; ++i) {
            Integer temp = final_mod.clone();
            final_mod *= temp;
        }

        auto P_final = compute_product_mod_parallel(
            ab_pairs, f_int, d, final_mod, false, max_threads);

        // Multiply P by f'(x)^2
        auto f_prime_int = compute_f_derivative_int(f_int, d);
        auto fli_final = compute_f_lead_inv(f_int, d, final_mod);
        auto f_prime_sq = poly_mul_mod(
            f_prime_int, f_prime_int, f_int, d, final_mod, fli_final);
        P_final = poly_mul_mod(P_final, f_prime_sq, f_int, d, final_mod, fli_final);

        // Newton iteration: S_{k+1} = S_k + T_k · (P - S_k²), T_{k+1} = T_k · (2 - 2S_{k+1}·T_k)
        for (size_t lift = 0; lift < num_lifts; ++lift) {
            Integer new_modulus = modulus.clone();
            new_modulus *= modulus;

            auto fli = compute_f_lead_inv(f_int, d, new_modulus);

            std::vector<Integer> P(d);
            for (uint32_t i = 0; i < d; ++i) {
                P[i] = P_final[i].clone();
                P[i] %= new_modulus;
            }

            auto S2 = poly_mul_mod(S, S, f_int, d, new_modulus, fli);
            auto residual = poly_sub_mod(P, S2, new_modulus);
            auto correction = poly_mul_mod(T, residual, f_int, d, new_modulus, fli);

            for (uint32_t i = 0; i < d; ++i) {
                S[i] += correction[i];
                S[i] %= new_modulus;
                if (S[i].is_negative()) S[i] += new_modulus;
            }

            // Update T
            std::vector<Integer> two_S_prime(d);
            for (uint32_t i = 0; i < d; ++i) {
                two_S_prime[i] = S[i].clone();
                two_S_prime[i] *= Integer(int64_t(2));
                two_S_prime[i] %= new_modulus;
            }
            auto two_S_T = poly_mul_mod(two_S_prime, T, f_int, d, new_modulus, fli);

            std::vector<Integer> factor(d);
            factor[0] = Integer(int64_t(2));
            factor[0] -= two_S_T[0];
            factor[0] %= new_modulus;
            if (factor[0].is_negative()) factor[0] += new_modulus;
            for (uint32_t i = 1; i < d; ++i) {
                factor[i] = two_S_T[i].clone();
                factor[i].negate();
                factor[i] %= new_modulus;
                if (factor[i].is_negative()) factor[i] += new_modulus;
            }
            T = poly_mul_mod(T, factor, f_int, d, new_modulus, fli);
            modulus = std::move(new_modulus);

            // Early invariant check at first 2 lifts to bail fast on divergence
            if (lift <= 1) {
                auto S2_early = poly_mul_mod(S, S, f_int, d, modulus, fli);
                bool early_ok = true;
                for (uint32_t i = 0; i < d; ++i) {
                    Integer p_i = P_final[i].clone();
                    p_i %= modulus;
                    if (S2_early[i].compare(p_i) != 0) {
                        early_ok = false;
                        break;
                    }
                }
                if (!early_ok) {
                    if (config_.verbose) {
                        std::cerr << "[Hensel-lift] p=" << p
                                  << " early invariant FAILED at lift " << lift << "\n";
                    }
                    result.ok = false;
                    return result;
                }
            }
        }

        result.coeffs = std::move(S);
        result.modulus = std::move(modulus);
        result.ok = true;
        return result;
    }

    /// Nguyen hybrid: K small primes + Hensel lift each + CRT + 2^(K-1) sign search
    [[nodiscard]] std::optional<Integer> compute_nguyen_hybrid(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            double target_bits,
            const Integer& product_at_m,
            const Integer& /* f_prime_m */,
            const Integer& f_prime_m_inv) const {

        uint32_t d = nf.degree();
        const Integer& n = nf.n();
        auto t0 = std::chrono::steady_clock::now();

        // Choose K: balance lift speed vs sign combo count.
        // K = number of inert primes for CRT. Sign combos = 2^(K-1).
        //
        // v19 重启 K=3 调研 (BACKLOG P1 DEFERRED):
        // 历史 Session 78 K=3 用 target_bits/K + 100 失败。分析:
        //   总 M 比特 = K · per_prime = target + K·100;K=3 仅 100 bit safety
        //   超过 target,但 target_bits 估算可能差 ~200 bit (大类群、复杂
        //   relation set),且 lift 重试也吃 safety。
        // 修复: per-prime safety 从 +100 提到 +200。总 safety: K·200。
        // 代价: 每 prime 多 lift 一轮 (~30% 单 prime lift 时间)。
        // 收益: K=3 给 4 sign combos vs K=2 的 2,但更重要的是 per-prime
        // bits 减少 (target/3 vs target/2) 让 lift 总开销下降。
        const size_t K = 3;

        // Find extra inert primes for retry on lift failure
        const size_t extra = 5;
        auto all_inert = find_inert_primes(nf, K + extra);
        if (all_inert.size() < K) {
            HENSEL_VERBOSE(std::cerr << "[Nguyen] Insufficient inert primes\n");
            return std::nullopt;
        }
        // Use first K as initial set
        std::vector<uint64_t> inert_primes(all_inert.begin(), all_inert.begin() + K);

        // Compute how many lifts each prime needs
        // After num_lifts doublings: precision = p^{2^num_lifts} ≈ 2^{log2(p) * 2^num_lifts}
        // Target per prime: target_bits/K + per-prime safety (v19: +200,见上方 K=3 注释)
        double per_prime_bits = target_bits / K + 200;
        std::vector<size_t> lifts_per_prime(K);
        for (size_t i = 0; i < K; ++i) {
            double log_p = std::log2(static_cast<double>(inert_primes[i]));
            size_t num_lifts = 0;
            double cur = log_p;
            while (cur < per_prime_bits) { cur *= 2; ++num_lifts; }
            lifts_per_prime[i] = num_lifts;
        }

        if (config_.verbose) {
            auto ms_find = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::cerr << "[Nguyen] " << K << " primes (";
            for (size_t i = 0; i < K; ++i)
                std::cerr << (i ? "," : "") << inert_primes[i];
            std::cerr << ") target=" << static_cast<size_t>(target_bits)
                      << " bits, per_prime=" << static_cast<size_t>(per_prime_bits)
                      << " bits, lifts=[";
            for (size_t i = 0; i < K; ++i)
                std::cerr << (i ? "," : "") << lifts_per_prime[i];
            std::cerr << "] found in " << ms_find << "ms\n";
        }

        // ---- Step 1: Parallel Hensel lift for each prime ----
        // Limit inner threads to hw/K to prevent oversubscription
        // (K outer threads × hw inner threads = K×hw >> hw cores)
        unsigned hw = std::thread::hardware_concurrency();
        size_t inner_threads = std::max(size_t(1), static_cast<size_t>(hw > 0 ? hw : 4) / K);

        std::vector<LiftResult> lifted(K);
        size_t next_spare = K;  // index into all_inert for replacement primes

        // Initial parallel lift
        {
            std::vector<std::thread> threads;
            threads.reserve(K);
            for (size_t i = 0; i < K; ++i) {
                threads.emplace_back([&, i]() {
                    lifted[i] = hensel_lift_single_prime(
                        inert_primes[i], ab_pairs, nf, lifts_per_prime[i],
                        inner_threads);
                });
            }
            for (auto& th : threads) th.join();
        }

        // Retry failed primes with replacements (sequential, one at a time)
        for (size_t i = 0; i < K; ++i) {
            while (!lifted[i].ok && next_spare < all_inert.size()) {
                if (config_.verbose) {
                    std::cerr << "[Nguyen] Lift failed for prime " << inert_primes[i]
                              << ", replacing with " << all_inert[next_spare] << "\n";
                }
                inert_primes[i] = all_inert[next_spare++];
                // Recompute lifts for replacement prime
                double log_p = std::log2(static_cast<double>(inert_primes[i]));
                size_t nl = 0;
                double cur = log_p;
                while (cur < per_prime_bits) { cur *= 2; ++nl; }
                lifts_per_prime[i] = nl;
                // Use all threads for single retry (no contention)
                lifted[i] = hensel_lift_single_prime(
                    inert_primes[i], ab_pairs, nf, lifts_per_prime[i],
                    static_cast<size_t>(hw > 0 ? hw : 4));
            }
            if (!lifted[i].ok) {
                if (config_.verbose)
                    std::cerr << "[Nguyen] All replacement primes exhausted for slot "
                              << i << "\n";
                return std::nullopt;
            }
        }

        if (config_.verbose) {
            auto ms_lift = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::cerr << "[Nguyen] All " << K << " lifts OK ("
                      << lifted[0].modulus.bit_length() << "-"
                      << lifted.back().modulus.bit_length() << " bit moduli) ["
                      << ms_lift << "ms total]\n";
        }

        // ---- Step 2: CRT combine + sign search ----
        // M = product of all per-prime moduli
        Integer M(int64_t(1));
        for (size_t i = 0; i < K; ++i) M *= lifted[i].modulus;

        // CRT basis: e_i = (M/m_i) * (M/m_i)^{-1} mod m_i
        std::vector<Integer> basis(K);
        for (size_t i = 0; i < K; ++i) {
            Integer Mi;
            mpz_divexact(Mi.get_mpz(), M.get_mpz(), lifted[i].modulus.get_mpz());
            Integer Mi_inv;
            mpz_invert(Mi_inv.get_mpz(), Mi.get_mpz(), lifted[i].modulus.get_mpz());
            basis[i] = Mi;
            basis[i] *= Mi_inv;
            basis[i] %= M;
        }

        // Initial CRT with all-positive signs
        std::vector<Integer> crt_val(d);
        for (uint32_t j = 0; j < d; ++j) {
            crt_val[j] = Integer(int64_t(0));
            for (size_t i = 0; i < K; ++i) {
                Integer term = lifted[i].coeffs[j].clone();
                term *= basis[i];
                crt_val[j] += term;
            }
            crt_val[j] %= M;
        }

        // Pre-compute delta[i][j] = (m_i - 2*s_{i,j}) * basis_i mod M
        std::vector<std::vector<Integer>> delta(K);
        for (size_t i = 0; i < K; ++i) {
            delta[i].reserve(d);
            for (uint32_t j = 0; j < d; ++j) {
                Integer v = lifted[i].modulus.clone();
                Integer ts = lifted[i].coeffs[j].clone();
                ts *= Integer(int64_t(2));
                v -= ts;
                v *= basis[i];
                v %= M;
                if (v.is_negative()) v += M;
                delta[i].push_back(std::move(v));
            }
        }

        // m^j mod N
        std::vector<Integer> mpow(d);
        mpow[0] = Integer(int64_t(1));
        for (uint32_t j = 1; j < d; ++j) {
            mpow[j] = mpow[j-1].clone();
            mpow[j] *= nf.m();
            mpow[j] %= n;
        }

        Integer Mhalf = M.clone();
        mpz_tdiv_q_2exp(Mhalf.get_mpz(), Mhalf.get_mpz(), 1);
        Integer M_mod_N = M.clone();
        M_mod_N %= n;

        // Incremental mod-N values for fast verification
        std::vector<Integer> crt_mod_N(d);
        for (uint32_t j = 0; j < d; ++j) {
            crt_mod_N[j] = crt_val[j].clone();
            crt_mod_N[j] %= n;
        }

        std::vector<std::vector<Integer>> delta_mod_N(K);
        for (size_t i = 0; i < K; ++i) {
            delta_mod_N[i].resize(d);
            for (uint32_t j = 0; j < d; ++j) {
                delta_mod_N[i][j] = delta[i][j].clone();
                delta_mod_N[i][j] %= n;
            }
        }

        // Verification lambda
        auto try_verify = [&]() -> std::optional<Integer> {
            Integer val(int64_t(0));
            for (uint32_t j = 0; j < d; ++j) {
                Integer c = crt_mod_N[j].clone();
                if (crt_val[j].compare(Mhalf) > 0) c -= M_mod_N;
                c %= n;
                if (c.is_negative()) c += n;
                c *= mpow[j];
                val += c;
                val %= n;  // intermediate reduction — avoid d·N² growth
            }
            if (val.is_negative()) val += n;
            val *= f_prime_m_inv;
            val %= n;
            if (val.is_negative()) val += n;
            return verify_and_return(val, product_at_m, n);
        };

        // Try all-positive
        auto result = try_verify();
        if (result) {
            HENSEL_VERBOSE(std::cerr << "[Nguyen] Verified at combo 0\n");
            return result;
        }

        // Gray code over K-1 primes (fix prime 0 as +)
        std::vector<bool> sgn(K, true);
        uint32_t total = 1u << (K - 1);

        for (uint32_t step = 1; step < total; ++step) {
            uint32_t flip = static_cast<uint32_t>(__builtin_ctz(step)) + 1;

            if (sgn[flip]) {
                for (uint32_t j = 0; j < d; ++j) {
                    crt_val[j] += delta[flip][j];
                    bool overflow = (crt_val[j].compare(M) >= 0);
                    if (overflow) crt_val[j] -= M;
                    crt_mod_N[j] += delta_mod_N[flip][j];
                    if (overflow) crt_mod_N[j] -= M_mod_N;
                    crt_mod_N[j] %= n;
                    if (crt_mod_N[j].is_negative()) crt_mod_N[j] += n;
                }
                sgn[flip] = false;
            } else {
                for (uint32_t j = 0; j < d; ++j) {
                    crt_val[j] -= delta[flip][j];
                    bool underflow = crt_val[j].is_negative();
                    if (underflow) crt_val[j] += M;
                    crt_mod_N[j] -= delta_mod_N[flip][j];
                    if (underflow) crt_mod_N[j] += M_mod_N;
                    crt_mod_N[j] %= n;
                    if (crt_mod_N[j].is_negative()) crt_mod_N[j] += n;
                }
                sgn[flip] = true;
            }

            result = try_verify();
            if (result) {
                if (config_.verbose) {
                    std::cerr << "[Nguyen] Verified at combo " << step
                              << "/" << total << "\n";
                }
                return result;
            }
        }

        if (config_.verbose) {
            std::cerr << "[Nguyen] No valid sign combo in " << total << " tried\n";
        }
        crt_sign_exhausted_ = true;
        return std::nullopt;
    }


    // ========================================================================
    // Classic Hensel lifting fallback
    // ========================================================================

    /// Classic Hensel lifting approach (fallback for small inputs or CRT failure)
    [[nodiscard]] std::optional<Integer> compute_hensel_lifting(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            double target_bits,
            const Integer& product_at_m,
            const Integer& /* f_prime_m */,
            const Integer& f_prime_m_inv) const {

        uint32_t d = nf.degree();
        const Integer& n = nf.n();

        // Find inert prime
        uint64_t p = config_.cached_inert_prime;
        if (p == 0) p = find_inert_prime(nf);
        if (p == 0) return std::nullopt;
        last_inert_prime_ = p;

        // Compute product and sqrt mod (f, p)
        auto f_mod_p = get_f_mod_p(nf, p);
        ModularPoly product_mod_p(1);
        for (const auto& [a, b] : ab_pairs) {
            std::vector<uint64_t> cs(2);
            int64_t am = a % static_cast<int64_t>(p);
            if (am < 0) am += static_cast<int64_t>(p);
            cs[0] = static_cast<uint64_t>(am);
            cs[1] = (p - (b % p)) % p;
            product_mod_p = ModularPoly::mul(product_mod_p, ModularPoly(std::move(cs)), f_mod_p, p);
        }
        if (product_mod_p.is_zero()) return std::nullopt;

        if (!ModularPoly::is_square(product_mod_p, f_mod_p, p)) return std::nullopt;
        auto sqrt_mod_p = ModularPoly::sqrt_tonelli_shanks(product_mod_p, f_mod_p, p);

        // Verify initial sqrt: sqrt^2 ≡ product mod (f, p)
        if (config_.verbose) {
            auto check = ModularPoly::mul(sqrt_mod_p, sqrt_mod_p, f_mod_p, p);
            bool init_ok = true;
            for (int i = 0; i <= std::max(check.degree(), product_mod_p.degree()); ++i) {
                if (check.coeff(i) != product_mod_p.coeff(i)) {
                    init_ok = false;
                    std::cerr << "[Hensel] INITIAL sqrt verification FAILED at coeff "
                              << i << ": got " << check.coeff(i) << " expected "
                              << product_mod_p.coeff(i) << " (p=" << p << ")\n";
                    break;
                }
            }
            if (init_ok) {
                std::cerr << "[Hensel] Initial sqrt verified OK (p=" << p << ")\n";
            }
        }

        // Multiply by f'(α)
        auto f_prime_mod_p = compute_f_derivative_mod_p(nf, p);
        sqrt_mod_p = ModularPoly::mul(
            sqrt_mod_p, ModularPoly(f_prime_mod_p), f_mod_p, p);

        // Compute lifts needed
        double log_p = std::log2(static_cast<double>(p));
        size_t base_lifts = 0;
        {
            double cur = log_p;
            while (cur < target_bits) { cur *= 2; ++base_lifts; }
        }

        for (int attempt = 0; attempt < 4; ++attempt) {
            size_t num_lifts = base_lifts + static_cast<size_t>(attempt);

            if (config_.verbose) {
                double mod_bits = log_p;
                for (size_t l = 0; l < num_lifts; ++l) mod_bits *= 2;
                std::cerr << "[Hensel-lift] attempt=" << attempt << " p=" << p
                          << " lifts=" << num_lifts
                          << " modulus_bits~=" << static_cast<size_t>(mod_bits) << "\n";
            }

            auto result_elem = hensel_lift_and_extract(
                sqrt_mod_p, ab_pairs, nf, p, num_lifts, d);
            if (!result_elem) continue;

            Integer Y_prime = nf.evaluate_at_m_mod_n(*result_elem);
            Integer Y = Y_prime.clone();
            Y *= f_prime_m_inv;
            Y %= n;
            if (Y.is_negative()) Y += n;

            auto verified = verify_and_return(Y, product_at_m, n);
            if (verified) return verified;

            if (config_.verbose) {
                std::cerr << "[Hensel-lift] Verification FAILED (attempt " << attempt << ")\n";
            }
        }

        return std::nullopt;
    }

    /// Core Hensel lifting: given sqrt mod p, lift to target precision and extract result
    [[nodiscard]] std::optional<NumberFieldElement> hensel_lift_and_extract(
            const ModularPoly& sqrt_mod_p,
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const NumberField& nf,
            uint64_t p,
            size_t num_lifts,
            uint32_t d) const {

        const Integer& n = nf.n();

        // Convert to Integer polynomial
        std::vector<Integer> S(d);
        for (uint32_t i = 0; i < d; ++i) {
            S[i] = Integer(static_cast<int64_t>(
                (i <= static_cast<uint32_t>(sqrt_mod_p.degree())) ? sqrt_mod_p.coeff(i) : 0));
        }

        // Get f polynomial as Integer vector
        std::vector<Integer> f_int(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            f_int[i] = nf.coeff(i).clone();
        }

        Integer modulus(static_cast<int64_t>(p));

        // Compute T₀ = (2·S₀)^{-1} mod (f, p) using Fermat
        // In F_{p^d}: a^{-1} = a^{p^d - 2}
        std::vector<Integer> T(d);
        {
            std::vector<uint64_t> two_s_mod(d), f_mod_p_vec(d + 1);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t si = S[i].to_uint64();
                two_s_mod[i] = (2 * si) % p;
            }
            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = nf.coeff(i).clone();
                c %= Integer(p);
                if (c.is_negative()) c += Integer(p);
                f_mod_p_vec[i] = c.to_uint64();
            }
            // Compute p^d - 2 using mpz_ui_pow_ui (single GMP call)
            Integer q_minus_2;
            mpz_ui_pow_ui(q_minus_2.get_mpz(), p, d);
            q_minus_2 -= Integer(int64_t(2));
            auto inv_mp = ModularPoly::power(ModularPoly(two_s_mod), q_minus_2, f_mod_p_vec, p);
            for (uint32_t i = 0; i < d; ++i) {
                uint64_t coeff_val = (i <= static_cast<uint32_t>(inv_mp.degree())) ? inv_mp.coeff(i) : 0;
                T[i] = Integer(coeff_val);
            }
        }

        // Pre-compute product at final precision ONCE.
        std::vector<Integer> P_final;
        if (num_lifts > 0) {
            Integer final_mod(static_cast<int64_t>(p));
            for (size_t i = 0; i < num_lifts; ++i) {
                Integer temp = final_mod.clone();
                final_mod *= temp;
            }

            if (config_.verbose) {
                std::cerr << "[Hensel] Pre-computing product (" << ab_pairs.size()
                          << " factors, " << final_mod.bit_length() << "-bit modulus)...\n";
            }

            P_final = compute_product_mod_parallel(
                ab_pairs, f_int, d, final_mod, config_.verbose);

            // Multiply P by f'(x)^2 to ensure sqrt ∈ Z[α] after lifting.
            auto f_prime_int = compute_f_derivative_int(f_int, d);
            auto fli_final = compute_f_lead_inv(f_int, d, final_mod);
            auto f_prime_sq = poly_mul_mod(f_prime_int, f_prime_int, f_int, d, final_mod, fli_final);
            P_final = poly_mul_mod(P_final, f_prime_sq, f_int, d, final_mod, fli_final);

            if (config_.verbose) {
                std::cerr << "[Hensel] Product pre-computed (with f'(α)^2 factor)\n";

                // Verify consistency: P_final mod p should match S^2 mod (f, p)
                // where S = sqrt_mod_p (which includes f'(α) factor)
                auto f_mod_p_check = get_f_mod_p(nf, p);
                ModularPoly S_check = sqrt_mod_p;
                auto S2_check_mp = ModularPoly::mul(S_check, S_check, f_mod_p_check, p);
                bool product_consistent = true;
                for (uint32_t i = 0; i < d; ++i) {
                    Integer pfi = P_final[i].clone();
                    Integer pp(static_cast<int64_t>(p));
                    Integer::mod(pfi, pfi, pp);
                    if (pfi.is_negative()) pfi += pp;
                    uint64_t pfi_u64 = pfi.to_uint64();
                    uint64_t s2i = (i <= static_cast<uint32_t>(S2_check_mp.degree()))
                                   ? S2_check_mp.coeff(i) : 0;
                    if (pfi_u64 != s2i) {
                        product_consistent = false;
                        std::cerr << "[Hensel] PRODUCT MISMATCH at coeff " << i
                                  << ": P_final%" << p << "=" << pfi_u64
                                  << " S^2%" << p << "=" << s2i << "\n";
                        break;
                    }
                }
                if (product_consistent) {
                    std::cerr << "[Hensel] Product consistent with S^2 mod p ✓\n";
                }
            }
        }

        // Hensel lifting: maintain S and T = (2S)^{-1} in parallel
        for (size_t lift = 0; lift < num_lifts; ++lift) {
            Integer new_modulus = modulus.clone();
            new_modulus *= modulus;  // modulus²

            // Pre-compute f_lead_inv for this round (all 5 poly_mul_mod share it)
            auto fli = compute_f_lead_inv(f_int, d, new_modulus);

            // Reduce pre-computed product to current precision
            std::vector<Integer> P(d);
            for (uint32_t i = 0; i < d; ++i) {
                P[i] = P_final[i].clone();
                P[i] %= new_modulus;
            }

            // S² mod (f, new_modulus)
            auto S2 = poly_mul_mod(S, S, f_int, d, new_modulus, fli);

            // residual = P - S²
            auto residual = poly_sub_mod(P, S2, new_modulus);

            // correction = T · residual mod (f, new_modulus)
            auto correction = poly_mul_mod(T, residual, f_int, d, new_modulus, fli);

            // S' = S + correction
            for (uint32_t i = 0; i < d; ++i) {
                S[i] += correction[i];
                S[i] %= new_modulus;
                if (S[i].is_negative()) S[i] += new_modulus;
            }

            // Update T: T' = T · (2 - 2S'·T) mod (f, new_modulus)
            std::vector<Integer> two_S_prime(d);
            for (uint32_t i = 0; i < d; ++i) {
                two_S_prime[i] = S[i].clone();
                two_S_prime[i] *= Integer(int64_t(2));
                two_S_prime[i] %= new_modulus;
            }
            auto two_S_T = poly_mul_mod(two_S_prime, T, f_int, d, new_modulus, fli);

            // 2 - 2S'·T
            std::vector<Integer> factor(d);
            factor[0] = Integer(int64_t(2));
            factor[0] -= two_S_T[0];
            factor[0] %= new_modulus;
            if (factor[0].is_negative()) factor[0] += new_modulus;
            for (uint32_t i = 1; i < d; ++i) {
                factor[i] = two_S_T[i].clone();
                factor[i].negate();
                factor[i] %= new_modulus;
                if (factor[i].is_negative()) factor[i] += new_modulus;
            }

            // T' = T · factor
            T = poly_mul_mod(T, factor, f_int, d, new_modulus, fli);

            modulus = std::move(new_modulus);

            if (config_.verbose && (lift == 0 || lift == num_lifts - 1 ||
                                    (num_lifts > 10 && lift % (num_lifts / 4) == 0))) {
                std::cerr << "[Hensel] lift " << lift << "/" << num_lifts
                          << " modulus_bits=" << modulus.bit_length() << "\n";
            }

            // Early invariant check at first 2 lifts to bail fast on divergence
            if (lift <= 1) {
                auto S2_early = poly_mul_mod(S, S, f_int, d, modulus, fli);
                bool early_ok = true;
                for (uint32_t i = 0; i < d; ++i) {
                    Integer p_i = P_final[i].clone();
                    p_i %= modulus;
                    if (S2_early[i].compare(p_i) != 0) {
                        early_ok = false;
                        if (config_.verbose) {
                            std::cerr << "[Hensel] Early invariant FAIL at lift "
                                      << lift << ": S^2[" << i << "] != P[" << i
                                      << "] mod p^k\n";
                        }
                        break;
                    }
                }
                if (!early_ok) return std::nullopt;  // Bail immediately
            }
        }

        // Verify Hensel invariant: S^2 ≡ P mod (f, modulus)
        if (config_.verbose && num_lifts > 0) {
            auto S2_check = poly_mul_mod(S, S, f_int, d, modulus);
            bool lift_ok = true;
            for (uint32_t i = 0; i < d; ++i) {
                Integer p_i = P_final[i].clone();
                p_i %= modulus;
                if (S2_check[i].compare(p_i) != 0) {
                    lift_ok = false;
                    std::cerr << "[Hensel] INVARIANT VIOLATION: S^2[" << i
                              << "] != P[" << i << "] mod p^k\n";
                    std::cerr << "  S^2[" << i << "] bits=" << S2_check[i].bit_length()
                              << " P[" << i << "] bits=" << p_i.bit_length() << "\n";
                    break;
                }
            }
            if (lift_ok) {
                std::cerr << "[Hensel] Lift invariant OK: S^2 ≡ P mod (f, p^k)\n";
            }
        }

        // Center coefficients and reduce mod N
        Integer half_mod = modulus.clone();
        mpz_tdiv_q_2exp(half_mod.get_mpz(), half_mod.get_mpz(), 1);

        // Diagnostic: print pre-centering and pre-mod-N coefficient sizes
        if (config_.verbose) {
            size_t max_pre_center = 0, max_post_center = 0;
            bool any_centered = false;
            for (uint32_t i = 0; i < d; ++i) {
                max_pre_center = std::max(max_pre_center, S[i].bit_length());
                Integer centered = S[i].clone();
                if (centered.compare(half_mod) > 0) {
                    centered -= modulus;
                    any_centered = true;
                }
                max_post_center = std::max(max_post_center, centered.bit_length());
            }
            std::cerr << "[Hensel] pre-center max bits=" << max_pre_center
                      << " post-center max bits=" << max_post_center
                      << " (centered=" << (any_centered ? "yes" : "no") << ")\n";

            // Evaluate S(m) mod N using Hensel coefficients (before centering)
            const Integer& nn = nf.n();
            const Integer& mm = nf.m();
            Integer s_at_m(int64_t(0));
            for (int i = static_cast<int>(d) - 1; i >= 0; --i) {
                s_at_m *= mm;
                s_at_m += S[i];
                s_at_m %= nn;
            }
            if (s_at_m.is_negative()) s_at_m += nn;
            Integer s2_at_m = s_at_m.clone();
            s2_at_m *= s_at_m;
            s2_at_m %= nn;
            if (s2_at_m.is_negative()) s2_at_m += nn;

            // Evaluate P_final(m) mod N
            Integer p_at_m(int64_t(0));
            for (int i = static_cast<int>(d) - 1; i >= 0; --i) {
                p_at_m *= mm;
                p_at_m += P_final[i];
                p_at_m %= nn;
            }
            if (p_at_m.is_negative()) p_at_m += nn;

            std::cerr << "[Hensel] φ(S)^2 mod N == φ(P_final) mod N ? "
                      << (s2_at_m.compare(p_at_m) == 0 ? "YES" : "NO") << "\n";
        }

        std::vector<Integer> result_coeffs(d);
        for (uint32_t i = 0; i < d; ++i) {
            result_coeffs[i] = S[i].clone();
            if (result_coeffs[i].compare(half_mod) > 0) {
                result_coeffs[i] -= modulus;
            }
            result_coeffs[i] %= n;
            if (result_coeffs[i].is_negative()) result_coeffs[i] += n;
        }

        return NumberFieldElement(std::move(result_coeffs));
    }

    /// Compute f'(x) mod p (derivative of the defining polynomial)
    [[nodiscard]] static std::vector<uint64_t> compute_f_derivative_mod_p(
            const NumberField& nf, uint64_t p) {
        uint32_t d = nf.degree();
        std::vector<uint64_t> f_prime(d);
        for (uint32_t i = 0; i < d; ++i) {
            Integer c = nf.coeff(i + 1).clone();
            c *= Integer(static_cast<int64_t>(i + 1));
            c %= Integer(p);
            if (c.is_negative()) c += Integer(p);
            f_prime[i] = c.to_uint64();
        }
        return f_prime;
    }

    /// Compute f'(x) as Integer polynomial (d coefficients, degree d-1)
    [[nodiscard]] static std::vector<Integer> compute_f_derivative_int(
            const std::vector<Integer>& f, uint32_t d) {
        std::vector<Integer> f_prime(d);
        for (uint32_t i = 0; i < d; ++i) {
            f_prime[i] = f[i + 1].clone();
            f_prime[i] *= Integer(static_cast<int64_t>(i + 1));
        }
        return f_prime;
    }

    /// Evaluate f'(m) mod N via Horner's method
    [[nodiscard]] static Integer evaluate_f_derivative_at_m(const NumberField& nf) {
        uint32_t d = nf.degree();
        const Integer& m = nf.m();
        const Integer& n = nf.n();

        // f'(x) = d·c_d·x^{d-1} + (d-1)·c_{d-1}·x^{d-2} + ... + c_1
        Integer result = nf.coeff(d).clone();
        result *= Integer(static_cast<int64_t>(d));
        result %= n;

        for (int i = static_cast<int>(d) - 1; i >= 1; --i) {
            result *= m;
            Integer term = nf.coeff(i).clone();
            term *= Integer(static_cast<int64_t>(i));
            result += term;
            result %= n;
        }
        if (result.is_negative()) result += n;
        return result;
    }

    /// Find a prime p where f(x) is irreducible mod p
    [[nodiscard]] uint64_t find_inert_prime(const NumberField& nf) const {
        uint64_t p = config_.prime_start;

        for (size_t attempts = 0; attempts < 100000; ++attempts) {
            p = next_prime(p);

            // Check f irreducible mod p (full Rabin test)
            auto f_mod = get_f_mod_p(nf, p);
            if (f_mod.back() == 0) continue;

            if (ModularPoly::is_irreducible(f_mod, p)) {
                return p;
            }
        }
        return 0;
    }

    /// Get f(x) coefficients mod p
    [[nodiscard]] static std::vector<uint64_t> get_f_mod_p(const NumberField& nf, uint64_t p) {
        uint32_t d = nf.degree();
        std::vector<uint64_t> f(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            Integer c = nf.coeff(i).clone();
            c %= Integer(p);
            if (c.is_negative()) c += Integer(p);
            f[i] = c.to_uint64();
        }
        return f;
    }

    /// Compute ∏(a_i - b_i·x) mod (f, modulus) using Integer polynomial arithmetic
    [[nodiscard]] static std::vector<Integer> compute_product_mod(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {

        // Pre-compute f_lead_inv once (avoid recomputing per-factor)
        auto fli = compute_f_lead_inv(f, d, modulus);

        // Start with 1
        std::vector<Integer> product(d);
        product[0] = Integer(int64_t(1));
        for (uint32_t i = 1; i < d; ++i) product[i] = Integer(int64_t(0));

        for (const auto& [a, b] : ab_pairs) {
            // Factor = a - b·x
            std::vector<Integer> factor(d);
            Integer a_mod(a);
            a_mod %= modulus;
            if (a_mod.is_negative()) a_mod += modulus;
            factor[0] = std::move(a_mod);

            if (d > 1) {
                Integer neg_b(static_cast<int64_t>(b));
                neg_b.negate();
                neg_b %= modulus;
                if (neg_b.is_negative()) neg_b += modulus;
                factor[1] = std::move(neg_b);
            }
            for (uint32_t i = 2; i < d; ++i) factor[i] = Integer(int64_t(0));

            product = poly_mul_mod(product, factor, f, d, modulus, fli);
        }

        return product;
    }

    /// Parallel product computation: splits factors across threads
    /// then combines partial products. Falls back to sequential for small n.
    /// @param max_threads_hint: 0 = auto (hw_concurrency), >0 = limit threads
    [[nodiscard]] static std::vector<Integer> compute_product_mod_parallel(
            const std::vector<std::pair<int64_t, uint64_t>>& ab_pairs,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus,
            bool verbose = false,
            size_t max_threads_hint = 0) {

        size_t n = ab_pairs.size();

        // Determine thread count (respect max_threads_hint for Nguyen shared-core)
        unsigned hw = std::thread::hardware_concurrency();
        size_t num_threads = (hw > 0) ? static_cast<size_t>(hw) : 4;
        if (max_threads_hint > 0) num_threads = std::min(num_threads, max_threads_hint);
        if (n < num_threads * 50) num_threads = 1;  // lower threshold for better utilization

        if (num_threads <= 1) {
            return compute_product_mod(ab_pairs, f, d, modulus);
        }

        if (verbose) {
            std::cerr << "[Hensel] Parallel product: " << n << " factors, "
                      << num_threads << " threads\n";
        }

        size_t chunk = (n + num_threads - 1) / num_threads;
        size_t actual_threads = (n + chunk - 1) / chunk;

        // Pre-compute f_lead_inv once (shared across all threads)
        auto fli = compute_f_lead_inv(f, d, modulus);

        std::vector<std::vector<Integer>> partials(actual_threads);
        std::vector<std::thread> threads;
        threads.reserve(actual_threads);

        for (size_t t = 0; t < actual_threads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, n);

            threads.emplace_back([&partials, &ab_pairs, &f, &modulus, &fli, d, t, start, end]() {
                std::vector<Integer> product(d);
                product[0] = Integer(int64_t(1));
                for (uint32_t i = 1; i < d; ++i) product[i] = Integer(int64_t(0));

                for (size_t j = start; j < end; ++j) {
                    auto [a, b] = ab_pairs[j];
                    std::vector<Integer> factor(d);
                    Integer a_mod(a);
                    a_mod %= modulus;
                    if (a_mod.is_negative()) a_mod += modulus;
                    factor[0] = std::move(a_mod);

                    if (d > 1) {
                        Integer neg_b(static_cast<int64_t>(b));
                        neg_b.negate();
                        neg_b %= modulus;
                        if (neg_b.is_negative()) neg_b += modulus;
                        factor[1] = std::move(neg_b);
                    }
                    for (uint32_t i = 2; i < d; ++i) factor[i] = Integer(int64_t(0));

                    product = poly_mul_mod(product, factor, f, d, modulus, fli);
                }

                partials[t] = std::move(product);
            });
        }

        for (auto& th : threads) th.join();

        // Combine partial products sequentially
        auto result = std::move(partials[0]);
        for (size_t t = 1; t < actual_threads; ++t) {
            result = poly_mul_mod(result, partials[t], f, d, modulus, fli);
        }

        return result;
    }

    /// Compute f_lead_inv = f[d]^{-1} mod modulus (for poly_mul_mod)
    [[nodiscard]] static Integer compute_f_lead_inv(
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {
        Integer f_lead_inv(int64_t(1));
        Integer f_d = f[d].clone();
        f_d %= modulus;
        if (f_d.is_negative()) f_d += modulus;
        if (!f_d.is_one()) {
            int ok = mpz_invert(f_lead_inv.get_mpz(), f_d.get_mpz(),
                                modulus.get_mpz());
            if (!ok) {
                // f[d] not invertible mod modulus — can happen in verbose
                // verification when modulus = p^k and p | f[d].
                // Return 1 as fallback (caller should check).
                return Integer(int64_t(1));
            }
        }
        return f_lead_inv;
    }

    /// Polynomial multiplication mod (f, modulus) — convenience overload
    [[nodiscard]] static std::vector<Integer> poly_mul_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus) {
        auto fli = compute_f_lead_inv(f, d, modulus);
        return poly_mul_mod(a, b, f, d, modulus, fli);
    }

    /// Polynomial multiplication mod (f, modulus) with pre-computed f_lead_inv
    /// Both inputs have degree < d, result has degree < d
    [[nodiscard]] static std::vector<Integer> poly_mul_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const std::vector<Integer>& f,
            uint32_t d,
            const Integer& modulus,
            const Integer& f_lead_inv) {

        // Multiply: result has degree up to 2d-2
        std::vector<Integer> result(2 * d - 1);
        for (size_t i = 0; i < result.size(); ++i) result[i] = Integer(int64_t(0));

        // 累加 d² 个 a[i]*b[j] 不做 mod(每个 < modulus²,d ≤ 8 时 result
        // 单系数 ≤ d·modulus²,GMP 自动扩存),最后对 2d-1 个系数一次性 mod。
        // 原代码每 inner iter 双 mod (term%=mod + result%=mod) = d²·2 = 50 次 mod
        // (d=5),新代码仅 2d-1 = 9 次 mod。Hensel lift 大循环 hot path。
        for (uint32_t i = 0; i < d; ++i) {
            if (a[i].is_zero()) continue;
            for (uint32_t j = 0; j < d; ++j) {
                if (b[j].is_zero()) continue;
                Integer term = a[i].clone();
                term *= b[j];
                result[i + j] += term;
            }
        }
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] %= modulus;
        }

        // Reduce mod f using pre-computed f_lead_inv
        for (int k = static_cast<int>(2 * d - 2); k >= static_cast<int>(d); --k) {
            Integer lead = std::move(result[k]);
            result[k] = Integer(int64_t(0));
            if (lead.is_zero()) continue;

            // Scale by inverse of leading coefficient
            Integer lead_scaled = lead.clone();
            if (!f_lead_inv.is_one()) {
                lead_scaled *= f_lead_inv;
                lead_scaled %= modulus;
            }

            // Subtract lead_scaled * f[0..d-1] from result[k-d..k-1]
            for (uint32_t i = 0; i < d; ++i) {
                Integer sub = lead_scaled.clone();
                sub *= f[i];
                sub %= modulus;
                result[k - d + i] -= sub;
                result[k - d + i] %= modulus;
                if (result[k - d + i].is_negative()) result[k - d + i] += modulus;
            }
        }

        // Trim to d coefficients
        result.resize(d);
        return result;
    }

    /// Polynomial subtraction mod modulus
    [[nodiscard]] static std::vector<Integer> poly_sub_mod(
            const std::vector<Integer>& a,
            const std::vector<Integer>& b,
            const Integer& modulus) {

        size_t n = std::max(a.size(), b.size());
        std::vector<Integer> result(n);
        for (size_t i = 0; i < n; ++i) {
            result[i] = (i < a.size()) ? a[i].clone() : Integer(int64_t(0));
            if (i < b.size()) {
                result[i] -= b[i];
                result[i] %= modulus;
                if (result[i].is_negative()) result[i] += modulus;
            }
        }
        return result;
    }

    // poly_inverse_mod / poly_inverse_mod_direct removed (dead code).
    // Hensel sqrt maintains T explicitly throughout the lift so the
    // ring-inverse helpers were never reached from any caller.

    /// Find next prime (with overflow guard)
    [[nodiscard]] static uint64_t next_prime(uint64_t n) {
        if (n >= UINT64_MAX - 2) return 0;
        n++;
        if (n <= 2) return 2;
        if (n % 2 == 0) {
            if (n == UINT64_MAX) return 0;
            n++;
        }
        while (true) {
            bool is_p = true;
            for (uint64_t i = 3; i * i <= n; ++i) {
                if (n % i == 0) { is_p = false; break; }
            }
            if (is_p) return n;
            if (n > UINT64_MAX - 2) return 0;
            n += 2;
        }
    }
};

} // namespace gnfs::sqrt
