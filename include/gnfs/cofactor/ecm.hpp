#pragma once

#include "../core/integer.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <optional>

namespace gnfs {
namespace cofactor {

using core::Integer;

/// ECM (Elliptic Curve Method) 余因子分解
/// 使用蒙哥马利曲线的 XZ 坐标系
/// 适用于分解中等大小的合数 (最多 ~50 位的因子)
class ECM {
public:
    /// ECM 配置
    struct Config {
        uint32_t num_curves;
        uint64_t B1;
        uint64_t B2;
        bool auto_params;

        Config() : num_curves(25), B1(10000), B2(1000000), auto_params(true) {}
    };

    /// 尝试用 ECM 分解 n
    /// @param n 要分解的合数
    /// @param config ECM 配置
    /// @return 找到的因子，如果失败返回 nullopt
    [[nodiscard]] static std::optional<Integer> factor(const Integer& n, Config config = Config()) {
        if (n.is_one() || n.is_probable_prime() > 0) {
            return std::nullopt;
        }

        // 自动调整参数
        if (config.auto_params) {
            size_t bits = n.bit_length();
            auto_tune(bits, config);
        }

        // Use n's low bits + random_device to avoid repeating the same curves
        std::random_device rd;
        uint64_t n_low = mpz_getlimbn(n.get_mpz(), 0);
        uint64_t seed = rd() ^ n_low;
        std::mt19937_64 rng(seed);

        for (uint32_t curve = 0; curve < config.num_curves; ++curve) {
            // 随机选择 sigma
            uint64_t sigma = (rng() % 1000000) + 6;

            auto result = try_curve(n, sigma, config.B1, config.B2);
            if (result) {
                return result;
            }
        }

        return std::nullopt;
    }

    /// 快速 ECM 用于小因子 (适合筛法余因子)
    /// 使用较少的曲线和较小的界
    [[nodiscard]] static std::optional<Integer> quick_factor(const Integer& n) {
        Config config;
        config.num_curves = 10;
        config.B1 = 2000;
        config.B2 = 50000;
        config.auto_params = false;
        return factor(n, config);
    }

private:
    /// 蒙哥马利曲线上的点 (仅 XZ 坐标)
    struct Point {
        Integer x;
        Integer z;

        Point() : x(int64_t(0)), z(int64_t(1)) {}
        Point(Integer x_, Integer z_) : x(std::move(x_)), z(std::move(z_)) {}
    };

    /// 自动调整 ECM 参数
    static void auto_tune(size_t cofactor_bits, Config& config) {
        // 基于期望因子大小选择参数
        // 参考 GMP-ECM 的经验值
        if (cofactor_bits <= 40) {
            config.B1 = 2000;
            config.B2 = 100000;
            config.num_curves = 15;
        } else if (cofactor_bits <= 60) {
            config.B1 = 11000;
            config.B2 = 1100000;
            config.num_curves = 25;
        } else if (cofactor_bits <= 80) {
            config.B1 = 50000;
            config.B2 = 12500000;
            config.num_curves = 40;
        } else if (cofactor_bits <= 100) {
            config.B1 = 250000;
            config.B2 = 128000000;
            config.num_curves = 60;
        } else if (cofactor_bits <= 130) {
            config.B1 = 1000000;
            config.B2 = 1000000000;
            config.num_curves = 100;
        } else {
            // 对非常大的余因子，使用更激进的参数
            config.B1 = 3000000;
            config.B2 = 5000000000ULL;
            config.num_curves = 200;
        }
    }

    /// 蒙哥马利倍点: 2P
    /// 使用标准 XZ-only doubling 公式
    static Point mont_double(const Point& P, const Integer& a24, const Integer& n) {
        // u = (x + z)^2, v = (x - z)^2
        Integer u = P.x.clone();
        u += P.z;
        u %= n;
        Integer u2 = u.clone();
        u2 *= u;
        u2 %= n;

        Integer v = P.x.clone();
        v -= P.z;
        if (v.is_negative()) v += n;
        v %= n;
        Integer v2 = v.clone();
        v2 *= v;
        v2 %= n;

        // x2 = u * v
        Integer x2 = u2.clone();
        x2 *= v2;
        x2 %= n;

        // w = u - v
        Integer w = u2.clone();
        w -= v2;
        if (w.is_negative()) w += n;
        w %= n;

        // z2 = w * (v + a24 * w)
        Integer t = a24.clone();
        t *= w;
        t %= n;
        t += v2;
        t %= n;

        Integer z2 = w.clone();
        z2 *= t;
        z2 %= n;

        return Point(std::move(x2), std::move(z2));
    }

    /// 蒙哥马利差分加法: P + Q (已知 P - Q)
    static Point mont_add(const Point& P, const Point& Q, const Point& diff, const Integer& n) {
        // u = (Px - Pz) * (Qx + Qz)
        Integer u = P.x.clone();
        u -= P.z;
        if (u.is_negative()) u += n;
        u %= n;
        Integer t1 = Q.x.clone();
        t1 += Q.z;
        t1 %= n;
        u *= t1;
        u %= n;

        // v = (Px + Pz) * (Qx - Qz)
        Integer v = P.x.clone();
        v += P.z;
        v %= n;
        Integer t2 = Q.x.clone();
        t2 -= Q.z;
        if (t2.is_negative()) t2 += n;
        t2 %= n;
        v *= t2;
        v %= n;

        // x = diff.z * (u + v)^2
        Integer sum = u.clone();
        sum += v;
        sum %= n;
        Integer sum2 = sum.clone();
        sum2 *= sum;
        sum2 %= n;
        Integer xr = diff.z.clone();
        xr *= sum2;
        xr %= n;

        // z = diff.x * (u - v)^2
        Integer dif = u.clone();
        dif -= v;
        if (dif.is_negative()) dif += n;
        dif %= n;
        Integer dif2 = dif.clone();
        dif2 *= dif;
        dif2 %= n;
        Integer zr = diff.x.clone();
        zr *= dif2;
        zr %= n;

        return Point(std::move(xr), std::move(zr));
    }

    /// 蒙哥马利标量乘法: k * P
    /// 使用 double-and-add (Montgomery ladder)
    static Point mont_mul(const Point& P, uint64_t k, const Integer& a24, const Integer& n) {
        if (k == 0) return Point(Integer(int64_t(0)), Integer(int64_t(1)));
        if (k == 1) return Point(P.x.clone(), P.z.clone());

        Point R0(P.x.clone(), P.z.clone());
        Point R1 = mont_double(P, a24, n);

        // 从最高有效位开始
        int bits = 63 - __builtin_clzll(k);
        for (int i = bits - 1; i >= 0; --i) {
            if ((k >> i) & 1) {
                R0 = mont_add(R0, R1, P, n);
                R1 = mont_double(R1, a24, n);
            } else {
                R1 = mont_add(R0, R1, P, n);
                R0 = mont_double(R0, a24, n);
            }
        }

        return R0;
    }

    /// 蒙哥马利标量乘法: k * P (Integer 版本)
    static Point mont_mul_big(const Point& P, const Integer& k, const Integer& a24, const Integer& n) {
        size_t bits = k.bit_length();
        if (bits == 0) return Point(Integer(int64_t(0)), Integer(int64_t(1)));
        if (bits == 1) return Point(P.x.clone(), P.z.clone());

        Point R0(P.x.clone(), P.z.clone());
        Point R1 = mont_double(P, a24, n);

        for (int i = static_cast<int>(bits) - 2; i >= 0; --i) {
            if (k.test_bit(i)) {
                R0 = mont_add(R0, R1, P, n);
                R1 = mont_double(R1, a24, n);
            } else {
                R1 = mont_add(R0, R1, P, n);
                R0 = mont_double(R0, a24, n);
            }
        }

        return R0;
    }

    /// 简单素数筛 (用于 Stage 1，bound 较小)
    static std::vector<uint64_t> sieve_primes(uint64_t bound) {
        std::vector<bool> is_prime(bound + 1, true);
        is_prime[0] = is_prime[1] = false;
        for (uint64_t i = 2; i * i <= bound; ++i) {
            if (is_prime[i]) {
                for (uint64_t j = i * i; j <= bound; j += i) {
                    is_prime[j] = false;
                }
            }
        }
        std::vector<uint64_t> primes;
        for (uint64_t i = 2; i <= bound; ++i) {
            if (is_prime[i]) primes.push_back(i);
        }
        return primes;
    }

    /// 分段筛法: 对 (low, high] 中的每个素数调用 callback
    /// callback 返回 false 表示提前终止
    /// 内存: O(√high + SEGMENT_SIZE) 而非 O(high)
    template<typename Callback>
    static void for_each_prime_in_range(uint64_t low, uint64_t high, Callback&& callback) {
        if (high <= low) return;

        // 筛出 ≤ √high 的小素数
        uint64_t sqrt_high = static_cast<uint64_t>(std::sqrt(static_cast<double>(high))) + 1;
        auto small_primes = sieve_primes(sqrt_high);

        // 分段处理，每段 ~1M entries = 128KB for vector<bool>
        constexpr uint64_t SEGMENT_SIZE = 1ULL << 20;

        for (uint64_t seg_lo = low + 1; seg_lo <= high; seg_lo += SEGMENT_SIZE) {
            uint64_t seg_hi = std::min(seg_lo + SEGMENT_SIZE - 1, high);
            uint64_t seg_len = seg_hi - seg_lo + 1;

            std::vector<bool> is_prime_seg(seg_len, true);

            // 用小素数标记合数
            for (uint64_t p : small_primes) {
                if (p * p > seg_hi) break;

                // 段内第一个 p 的倍数
                uint64_t start = ((seg_lo + p - 1) / p) * p;
                if (start == p) start += p;  // 不标记 p 本身

                for (uint64_t j = start; j <= seg_hi; j += p) {
                    is_prime_seg[j - seg_lo] = false;
                }
            }

            // 处理 seg_lo <= 1 的边界情况
            if (seg_lo <= 1) {
                for (uint64_t v = seg_lo; v <= std::min(uint64_t(1), seg_hi); ++v) {
                    is_prime_seg[v - seg_lo] = false;
                }
            }

            // 回调每个素数，返回 false 则提前终止
            for (uint64_t i = 0; i < seg_len; ++i) {
                if (is_prime_seg[i]) {
                    if (!callback(seg_lo + i)) return;
                }
            }
        }
    }

    /// 尝试一条曲线
    [[nodiscard]] static std::optional<Integer> try_curve(
            const Integer& n, uint64_t sigma, uint64_t B1, uint64_t B2) {

        // Suyama's parametrization
        Integer u(static_cast<unsigned long long>(sigma * sigma - 5));
        u %= n;
        if (u.is_negative()) u += n;

        Integer v(static_cast<unsigned long long>(4 * sigma));
        v %= n;
        if (v.is_negative()) v += n;

        // 起始点
        Integer x0 = u.clone();
        x0 *= u;
        x0 %= n;
        x0 *= u;
        x0 %= n;  // u^3

        Integer z0 = v.clone();
        z0 *= v;
        z0 %= n;
        z0 *= v;
        z0 %= n;  // v^3

        // a24 = (v - u)^3 * (3u + v) / (16 * u^3 * v) - 2
        // 简化: 直接计算 a24 = ((v-u)^3 * (3u+v)) * inverse(16*u^3*v) - 2
        // 如果逆元不存在，我们就找到了因子!
        Integer diff = v.clone();
        diff -= u;
        if (diff.is_negative()) diff += n;
        diff %= n;

        Integer diff3 = diff.clone();
        diff3 *= diff;
        diff3 %= n;
        diff3 *= diff;
        diff3 %= n;

        Integer sum3u_v = u.clone();
        sum3u_v *= Integer(int64_t(3));
        sum3u_v += v;
        sum3u_v %= n;

        Integer numerator = diff3.clone();
        numerator *= sum3u_v;
        numerator %= n;

        Integer denom = x0.clone();
        denom *= v;
        denom %= n;
        denom *= Integer(int64_t(16));
        denom %= n;

        // 计算逆元
        Integer g = core::gcd(denom.clone(), n);
        if (!g.is_one() && g.compare(n) != 0) {
            return g;  // 找到因子!
        }
        if (g.compare(n) == 0) {
            return std::nullopt;  // 曲线退化
        }

        Integer denom_inv = core::mod_inverse(denom, n);
        if (denom_inv.is_zero()) {
            return std::nullopt;
        }

        Integer a24 = numerator.clone();
        a24 *= denom_inv;
        a24 %= n;

        // a24 = (a + 2) / 4, 所以实际的 a24 已经是我们需要的形式

        Point Q(std::move(x0), std::move(z0));

        // === Stage 1: 计算 k*Q，其中 k = ∏ p^{floor(log_p(B1))} ===
        auto primes = sieve_primes(B1);

        for (uint64_t p : primes) {
            // 计算 p^e <= B1 的最大 e
            uint64_t pk = p;
            while (pk <= B1 / p) pk *= p;  // 避免溢出

            Q = mont_mul(Q, pk, a24, n);

            // 定期检查 gcd
            if (p % 100 == 97) {  // 每 ~100 个素数检查一次
                Integer g2 = core::gcd(Q.z.clone(), n);
                if (!g2.is_one() && g2.compare(n) != 0) {
                    return g2;
                }
                if (g2.compare(n) == 0) {
                    return std::nullopt;  // 太多因子被消除
                }
            }
        }

        // Stage 1 最终检查
        Integer g_final = core::gcd(Q.z.clone(), n);
        if (!g_final.is_one() && g_final.compare(n) != 0) {
            return g_final;
        }
        if (g_final.compare(n) == 0) {
            return std::nullopt;
        }

        // === Stage 2: 标准续步 ===
        // 检查 B1 < p <= B2 的素数
        // 使用 baby-step giant-step 风格优化
        if (B2 > B1) {
            auto stage2_result = stage2(Q, n, a24, B1, B2);
            if (stage2_result) {
                return stage2_result;
            }
        }

        return std::nullopt;
    }

    /// Stage 2: 标准续步（使用分段筛法，内存安全）
    /// 当累积 gcd == n 时回退到逐素数检查，避免因子丢失
    [[nodiscard]] static std::optional<Integer> stage2(
            const Point& Q0, const Integer& n, const Integer& a24,
            uint64_t B1, uint64_t B2) {

        // 使用分段筛法逐步处理 (B1, B2] 中的素数
        Integer accum(int64_t(1));
        Point Qcurr(Q0.x.clone(), Q0.z.clone());
        uint64_t check_interval = 0;
        std::optional<Integer> found;

        // Save checkpoint for backtracking on gcd==n
        Point checkpoint(Q0.x.clone(), Q0.z.clone());
        std::vector<uint64_t> batch_primes;
        batch_primes.reserve(128);

        for_each_prime_in_range(B1, B2, [&](uint64_t p) -> bool {
            Qcurr = mont_mul(Qcurr, p, a24, n);
            batch_primes.push_back(p);

            // 累积 z 坐标
            accum *= Qcurr.z;
            accum %= n;

            ++check_interval;
            if (check_interval >= 100) {
                Integer g = core::gcd(accum.clone(), n);
                if (!g.is_one() && g.compare(n) != 0) {
                    found = std::move(g);
                    return false;  // 找到因子，停止
                }
                if (g.compare(n) == 0) {
                    // gcd == n: backtrack — retry this batch individually
                    Point Q_retry(checkpoint.x.clone(), checkpoint.z.clone());
                    for (uint64_t bp : batch_primes) {
                        Q_retry = mont_mul(Q_retry, bp, a24, n);
                        Integer gi = core::gcd(Q_retry.z.clone(), n);
                        if (!gi.is_one() && gi.compare(n) != 0) {
                            found = std::move(gi);
                            return false;
                        }
                    }
                    // All individual gcd's were 1 or n — give up on this batch
                    // (extremely rare: both factors found in same batch)
                }
                // Reset for next batch
                accum = Integer(int64_t(1));
                checkpoint = Point(Qcurr.x.clone(), Qcurr.z.clone());
                batch_primes.clear();
                check_interval = 0;
            }
            return true;  // 继续
        });

        if (found) return found;

        // 最终检查
        Integer g = core::gcd(accum.clone(), n);
        if (!g.is_one() && g.compare(n) != 0) {
            return g;
        }
        if (g.compare(n) == 0 && !batch_primes.empty()) {
            // Backtrack the final batch
            Point Q_retry(checkpoint.x.clone(), checkpoint.z.clone());
            for (uint64_t bp : batch_primes) {
                Q_retry = mont_mul(Q_retry, bp, a24, n);
                Integer gi = core::gcd(Q_retry.z.clone(), n);
                if (!gi.is_one() && gi.compare(n) != 0) {
                    return gi;
                }
            }
        }

        return std::nullopt;
    }
};

} // namespace cofactor
} // namespace gnfs
