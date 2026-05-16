#pragma once

#include "../core/polynomial_context.hpp"
#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"
#include "lattice_sieve.hpp"  // SieveCandidate, SieveResult, SieveParams

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace gnfs::sieve {

using core::PolynomialContext;
using factor_base::FactorBase;

/// Line Sieve — simple sieve for small N (< 50 digits)
///
/// Operates directly in (a, b) space without Special-Q lattice reduction.
/// For each b in [1, b_max], sieves a in [a_min, a_max] by subtracting
/// log_p at positions where p | (a - b*m) (rational) or p | N(a,b) (algebraic).
///
/// Simpler than lattice sieve: no SQ loop, no lattice basis computation,
/// no grid coordinate transformation. Good for N < 50 digits where FB is small.
///
/// NOTE: 当前未集成到 Pipeline。小 N (≤80 bit) 路径走 TrialDivision/PollardRho,
/// 中 N 走 SIQS,大 N 走 Lattice Sieve。LineSieve 仅有 test_line_sieve 覆盖。
/// auto_params 在 ≤20-bit 时给的搜索区域 (b_max=200, a_max=5000) 太小,实际
/// GNFS 上不可能产关系。保留代码仅为算法对照,生产路径请勿调用。
class [[deprecated("LineSieve 未集成到 Pipeline; 用 LatticeSieve 或 SIQS")]] LineSieve {
public:
    struct Params {
        int64_t a_min = -100000;
        int64_t a_max = 100000;
        int64_t b_max = 1000;
        uint16_t threshold = 0;     // Combined threshold (0 = auto from SieveParams)
        uint8_t log_scale = core::SIEVE_LOG_SCALE;
    };

    LineSieve(const PolynomialContext& ctx, const FactorBase& fb)
        : ctx_(ctx), fb_(fb) {}

    /// Sieve a range of (a, b) pairs and return candidates
    [[nodiscard]] SieveResult sieve(const Params& params) const {
        SieveResult result;
        result.special_q = SpecialQ{0, 0, 0};  // No special-Q
        // Heuristic: ~1% of (a,b) cells pass smoothness threshold.
        const int64_t a_range = params.a_max - params.a_min + 1;
        result.candidates.reserve(static_cast<size_t>(a_range * params.b_max) / 100);
        if (a_range <= 0 || params.b_max <= 0) return result;

        const size_t width = static_cast<size_t>(a_range);

        // Estimate initial log value for threshold calculation
        double m_val = ctx_.m().to_double();
        double typical_a = static_cast<double>(params.a_max) / 2.0;
        double typical_b = static_cast<double>(params.b_max) / 2.0;
        double rat_log = std::log2(std::max(1.0, std::abs(typical_a - typical_b * m_val)));
        double alg_log = std::log2(std::max(1.0, std::pow(std::max(typical_a, 1.0), ctx_.degree())));
        double combined_log = (rat_log + alg_log) * params.log_scale;
        uint16_t init_val = static_cast<uint16_t>(std::min(combined_log, 65535.0));

        uint16_t threshold = params.threshold;
        if (threshold == 0) {
            // Auto: use ~60% of init_val as threshold
            threshold = static_cast<uint16_t>(init_val * 0.6);
        }
        if (init_val <= threshold) return result;
        uint16_t eff_threshold = static_cast<uint16_t>(init_val - threshold);

        // Sieve array for one row (a-line)
        std::vector<uint16_t> sieve_array(width, 0);

        // Precompute log_p values
        auto compute_log_p = [&](uint32_t p) -> uint16_t {
            if (p <= 1) return 0;
            return static_cast<uint16_t>(std::log2(static_cast<double>(p)) * params.log_scale);
        };

        // Process each b value
        for (int64_t b = 1; b <= params.b_max; ++b) {
            // Clear sieve array
            std::memset(sieve_array.data(), 0, width * sizeof(uint16_t));

            // Rational side: p | (a - b*m) → a ≡ b*m (mod p)
            for (const auto& rp : fb_.rational()) {
                uint32_t p = rp.p;
                uint16_t log_p = compute_log_p(p);

                // Compute starting position: a ≡ b*m (mod p)
                uint64_t bm_mod_p;
                if (ctx_.m().fits_uint64()) {
                    bm_mod_p = (static_cast<__uint128_t>(b % p) * (ctx_.m().to_uint64() % p)) % p;
                } else {
                    core::Integer bmod(static_cast<unsigned long long>(b % p));
                    core::Integer p_int(static_cast<unsigned long long>(p));
                    core::Integer mmod;
                    core::Integer::mod(mmod, ctx_.m(), p_int);
                    bmod *= mmod;
                    bmod %= p_int;
                    bm_mod_p = bmod.to_uint64();
                }

                // Find first a >= a_min with a ≡ bm (mod p)
                int64_t a_start = static_cast<int64_t>(bm_mod_p);
                // Adjust to be >= a_min
                if (a_start < params.a_min) {
                    int64_t diff = params.a_min - a_start;
                    int64_t steps = (diff + p - 1) / p;
                    a_start += steps * p;
                } else if (a_start > params.a_min) {
                    // Reduce to first valid position
                    int64_t diff = a_start - params.a_min;
                    int64_t steps = diff / p;
                    a_start -= steps * p;
                    if (a_start < params.a_min) a_start += p;
                }

                // Stride through sieve array
                for (int64_t a = a_start; a <= params.a_max; a += p) {
                    size_t idx = static_cast<size_t>(a - params.a_min);
                    if (idx < width) sieve_array[idx] += log_p;
                }
            }

            // Algebraic side: for each (p, r), p | (a - b*r) → a ≡ b*r (mod p)
            const size_t sieve_count = fb_.sieve_algebraic_count();
            const auto& algebraics = fb_.algebraic();
            for (size_t ai = 0; ai < sieve_count; ++ai) {
                const auto& ap = algebraics[ai];
                if (ap.is_projective()) continue;

                uint32_t p = ap.p;
                uint16_t log_p = compute_log_p(p);
                uint32_t r = ap.r;

                uint64_t br_mod_p = (static_cast<__uint128_t>(b % p) * (r % p)) % p;

                int64_t a_start = static_cast<int64_t>(br_mod_p);
                if (a_start < params.a_min) {
                    int64_t diff = params.a_min - a_start;
                    int64_t steps = (diff + p - 1) / p;
                    a_start += steps * p;
                } else if (a_start > params.a_min) {
                    int64_t diff = a_start - params.a_min;
                    int64_t steps = diff / p;
                    a_start -= steps * p;
                    if (a_start < params.a_min) a_start += p;
                }

                for (int64_t a = a_start; a <= params.a_max; a += p) {
                    size_t idx = static_cast<size_t>(a - params.a_min);
                    if (idx < width) sieve_array[idx] += log_p;
                }
            }

            // Collect candidates from this b-line
            for (size_t idx = 0; idx < width; ++idx) {
                if (sieve_array[idx] >= eff_threshold) {
                    int64_t a = params.a_min + static_cast<int64_t>(idx);
                    if (a == 0) continue;  // Skip a=0 (trivial)
                    if (std::gcd(std::abs(a), b) != 1) continue;  // gcd(a,b) must be 1

                    SieveCandidate cand;
                    cand.i = static_cast<int32_t>(idx);
                    cand.j = static_cast<int32_t>(b);
                    cand.a = a;
                    cand.b = static_cast<uint64_t>(b);
                    cand.residual = static_cast<uint8_t>(
                        std::min(static_cast<uint16_t>(255),
                                 static_cast<uint16_t>(init_val - sieve_array[idx])));
                    result.candidates.push_back(cand);
                }
            }
        }

        result.sieved_positions = width * static_cast<size_t>(params.b_max);
        return result;
    }

    /// Auto-parameterize from GNFSParams
    [[nodiscard]] static Params auto_params(size_t bits) {
        Params p;
        if (bits <= 20) {
            p.a_min = -5000; p.a_max = 5000; p.b_max = 200;
        } else if (bits <= 40) {
            p.a_min = -50000; p.a_max = 50000; p.b_max = 500;
        } else if (bits <= 80) {
            p.a_min = -200000; p.a_max = 200000; p.b_max = 2000;
        } else {
            p.a_min = -1000000; p.a_max = 1000000; p.b_max = 5000;
        }
        return p;
    }

private:
    const PolynomialContext& ctx_;
    const FactorBase& fb_;
};

} // namespace gnfs::sieve
