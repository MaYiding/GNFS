#pragma once

#include "../core/integer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <optional>

namespace gnfs::cofactor {

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

        // Pre-compute primes once for all curves (was per-curve: 200×600KB)
        auto cached_primes = sieve_primes(config.B1);

        for (uint32_t curve = 0; curve < config.num_curves; ++curve) {
            // 随机选择 sigma
            uint64_t sigma = (rng() % 1000000) + 6;

            auto result = try_curve(n, sigma, config.B1, config.B2, cached_primes);
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

        Point() : x(), z(1) {}
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
            // >130-bit 余因子(~40+ digit 素因子) Stage 2 极慢(B2=5e9 需要
            // 数分钟/曲线)。理论上 GNFS pipeline 应在更早把这类大余因子
            // 直接拒掉(大于 algebraic_bound²的 cofactor 不可能光滑),不
            // 走到 ECM。此分支只是兜底,避免 num_curves 等参数完全失配。
            config.B1 = 3000000;
            config.B2 = 5000000000ULL;
            config.num_curves = 200;
        }
    }

    /// 蒙哥马利倍点: 2P
    /// 使用标准 XZ-only doubling 公式
    /// v22: thread_local workspace 复用所有内部 Integer buffers (mpz_set 不 init)
    /// Stage 1 调用 ~600K 次/曲线, 25 曲线 = 15M 调用/cofactor candidate
    static Point mont_double(const Point& P, const Integer& a24, const Integer& n) {
        thread_local Integer u, u2, v, v2, w, t;
        Integer x2, z2;  // 返回值 — Point ctor 通过 move 接收

        // u = (x + z) mod n; u2 = u^2
        u = P.x;
        u += P.z;
        u %= n;
        u2 = u;
        u2 *= u;
        u2 %= n;

        // v = (x - z) mod n; v2 = v^2
        v = P.x;
        v -= P.z;
        if (v.is_negative()) v += n;
        v %= n;
        v2 = v;
        v2 *= v;
        v2 %= n;

        // x2 = u^2 * v^2
        x2 = u2;
        x2 *= v2;
        x2 %= n;

        // w = u^2 - v^2
        w = u2;
        w -= v2;
        if (w.is_negative()) w += n;
        w %= n;

        // z2 = w * (v^2 + a24 * w)
        t = a24;
        t *= w;
        t %= n;
        t += v2;
        t %= n;

        z2 = w;
        z2 *= t;
        z2 %= n;

        return Point(std::move(x2), std::move(z2));
    }

    /// 蒙哥马利差分加法: P + Q (已知 P - Q)
    /// v22: thread_local workspace 复用
    static Point mont_add(const Point& P, const Point& Q, const Point& diff, const Integer& n) {
        thread_local Integer u, v, t1, t2, sum, sum2, dif, dif2;
        Integer xr, zr;  // 返回值

        // u = (Px - Pz) * (Qx + Qz)
        u = P.x;
        u -= P.z;
        if (u.is_negative()) u += n;
        u %= n;
        t1 = Q.x;
        t1 += Q.z;
        t1 %= n;
        u *= t1;
        u %= n;

        // v = (Px + Pz) * (Qx - Qz)
        v = P.x;
        v += P.z;
        v %= n;
        t2 = Q.x;
        t2 -= Q.z;
        if (t2.is_negative()) t2 += n;
        t2 %= n;
        v *= t2;
        v %= n;

        // xr = diff.z * (u + v)^2
        sum = u;
        sum += v;
        sum %= n;
        sum2 = sum;
        sum2 *= sum;
        sum2 %= n;
        xr = diff.z;
        xr *= sum2;
        xr %= n;

        // zr = diff.x * (u - v)^2
        dif = u;
        dif -= v;
        if (dif.is_negative()) dif += n;
        dif %= n;
        dif2 = dif;
        dif2 *= dif;
        dif2 %= n;
        zr = diff.x;
        zr *= dif2;
        zr %= n;

        return Point(std::move(xr), std::move(zr));
    }

    /// 蒙哥马利标量乘法: k * P
    /// 使用 double-and-add (Montgomery ladder)
    static Point mont_mul(const Point& P, uint64_t k, const Integer& a24, const Integer& n) {
        if (k == 0) return Point();  // default ctor: (0, 1)
        if (k == 1) return P;  // implicit copy ctor (Integer copy ctor)

        Point R0 = P;
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
        if (bits == 0) return Point();  // default ctor: (0, 1)
        if (bits == 1) return P;  // implicit copy ctor

        Point R0 = P;
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
        // π(bound) ≈ bound / ln(bound) — reserve to avoid log(n) reallocs.
        primes.reserve(static_cast<size_t>(bound / std::max(std::log(static_cast<double>(bound)), 1.0)));
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

    /// 辗转相除求 gcd (小整数)
    static constexpr uint64_t gcd_u64(uint64_t a, uint64_t b) {
        while (b) { uint64_t t = b; b = a % b; a = t; }
        return a;
    }

    /// 尝试一条曲线
    [[nodiscard]] static std::optional<Integer> try_curve(
            const Integer& n, uint64_t sigma, uint64_t B1, uint64_t B2,
            const std::vector<uint64_t>& primes_cache = {}) {

        // Suyama's parametrization 要求 sigma >= 6,否则 sigma²-5 在 uint64
        // 下下溢成巨大值,数学上得不到有效曲线。
        assert(sigma >= 6 && "ECM Suyama: sigma must be >= 6");

        // Integer(unsigned long long) 总是非负,is_negative() 永不为 true。
        Integer u(static_cast<unsigned long long>(sigma * sigma - 5));
        u %= n;

        Integer v(static_cast<unsigned long long>(4 * sigma));
        v %= n;

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
        sum3u_v *= int64_t(3);  // mpz_mul_si direct
        sum3u_v += v;
        sum3u_v %= n;

        Integer numerator = diff3.clone();
        numerator *= sum3u_v;
        numerator %= n;

        Integer denom = x0.clone();
        denom *= v;
        denom %= n;
        denom *= int64_t(16);  // mpz_mul_si direct
        denom %= n;

        // 计算逆元 (v22: gcd 取 const& 无需 clone; 单次 compare(n) 缓存)
        Integer g = core::gcd(denom, n);
        if (!g.is_one()) {
            // g > 1 → g == n (退化) 或 1 < g < n (lucky factor)
            if (g.compare(n) == 0) return std::nullopt;
            return g;
        }

        Integer denom_inv = core::mod_inverse(denom, n);
        if (denom_inv.is_zero()) {
            return std::nullopt;
        }

        Integer a24 = numerator.clone();
        a24 *= denom_inv;
        a24 %= n;

        // Suyama 参数化: A = (v-u)³(3u+v)/(4u³v) - 2, 其中 u=σ²-5, v=4σ。
        // (A+2)/4 = (v-u)³(3u+v) / (16 u³ v) — 上面 numerator/denom 正是这个表达。
        // 即 numerator * denom_inv = (A+2)/4 = a24 已为 mont_double 所需形式,
        // 无需额外 -2 或 /4。隐式由 test_ecm_quick.cpp 10d–55d 全部成功验证。

        Point Q(std::move(x0), std::move(z0));

        // === Stage 1: 计算 k*Q，其中 k = ∏ p^{floor(log_p(B1))} ===
        // factor() always passes primes_cache. The empty-cache branch existed
        // as a fallback for "direct callers"; grep shows there are none, so
        // every empty cache here is a bug. Assert tight so silent re-sieving
        // (O(B1·log log B1) per curve) can't sneak back in.
        assert(!primes_cache.empty() && "ECM::try_curve requires non-empty primes_cache");
        const auto& primes = primes_cache;

        for (uint64_t p : primes) {
            // 计算 p^e <= B1 的最大 e
            uint64_t pk = p;
            while (pk <= B1 / p) pk *= p;  // 避免溢出

            Q = mont_mul(Q, pk, a24, n);

            // 定期检查 gcd (v22: gcd 无需 clone; 单次 compare(n))
            if (p % 100 == 97) {  // 每 ~100 个素数检查一次
                Integer g2 = core::gcd(Q.z, n);
                if (!g2.is_one()) {
                    if (g2.compare(n) == 0) return std::nullopt;  // 太多因子被消除
                    return g2;
                }
            }
        }

        // Stage 1 最终检查 (v22: gcd 无需 clone; 单次 compare(n) 缓存)
        Integer g_final = core::gcd(Q.z, n);
        if (!g_final.is_one()) {
            if (g_final.compare(n) == 0) return std::nullopt;
            return g_final;
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

    /// Stage 2: Baby-Step Giant-Step 优化
    /// 将素数 p ∈ (B1, B2] 表示为 p = j·D ± d，D = 2310 = 2·3·5·7·11
    /// 预计算 baby steps d*Q (φ(D)=480 个点)，差分加法链推进 giant steps j·D*Q
    /// 检测: 若 p*Q = O (mod factor)，则 j·D*Q 和 d*Q 同 x 坐标，
    ///        cross product X_j·Z_d - X_d·Z_j ≡ 0 (mod factor)
    /// 复杂度: O(D) baby + O((B2-B1)/D) giant 曲线运算 + O(φ(D)·(B2-B1)/D) 模乘
    [[nodiscard]] static std::optional<Integer> stage2(
            const Point& Q0, const Integer& n, const Integer& a24,
            uint64_t B1, uint64_t B2) {

        constexpr uint64_t D = 2310;  // 2·3·5·7·11, φ(D)=480

        // 小范围不值得 BSGS 开销，回退朴素实现
        if (B2 - B1 < D * 3) {
            return stage2_naive(Q0, n, a24, B1, B2);
        }

        // === Phase 1: Baby steps — compute d*Q for d coprime to D ===
        struct BabyStep { Integer x; Integer z; };
        std::vector<BabyStep> baby;
        baby.reserve(480);

        // 增量链: (k+1)*Q = mont_add(k*Q, Q, (k-1)*Q)
        Point Q_one = Q0;                             // Q (constant base) — copy ctor
        Point km2 = Q0;                                // (k-2)*Q, starts as 1*Q
        Point km1 = mont_double(Q0, a24, n);          // (k-1)*Q, starts as 2*Q

        // d=1: gcd(1, 2310) = 1
        baby.push_back({Q0.x.clone(), Q0.z.clone()});

        for (uint64_t k = 3; k <= D; ++k) {
            Point curr = mont_add(km1, Q_one, km2, n);
            if (gcd_u64(k, D) == 1) {
                baby.push_back({curr.x.clone(), curr.z.clone()});
            }
            km2 = std::move(km1);
            km1 = std::move(curr);
        }
        // 循环结束后 km1 = D*Q
        Point Q_D(km1.x.clone(), km1.z.clone());

        // === Phase 2: Giant steps ===
        uint64_t j_lo = B1 / D;                  // 最小 j 使得 j*D ≥ B1 - D (covers B1-adjacent primes)
        // j_lo=0 (B1 < D) 会让 mont_mul(Q0, 0) 返回无穷远点 (0:1),
        // accumulate_step 计算 G.x * b.z - b.x * G.z = -b.x · 1 = -b.x,
        // 把 baby step 的随机数据当成 cross product 累入,污染 GCD。
        // 后面 G_curr 用 (j_lo+1)*D 也会跳过 [B1, D] 区间的素数。
        // 修复:把 j_lo 上调到 1,保证 j_lo*D ≥ D > B1 时仍覆盖 D 以下素数已在 Stage1 处理。
        if (j_lo == 0) j_lo = 1;
        uint64_t j_hi = (B2 + D - 1) / D;       // 最大 j 使得 (j-1)*D < B2
        if (j_lo > j_hi) return std::nullopt;

        // 两次 mont_mul 初始化差分链
        Point G_prev = mont_mul(Q0, j_lo * D, a24, n);
        Point G_curr = (j_lo < j_hi)
            ? mont_mul(Q0, (j_lo + 1) * D, a24, n)
            : Point();

        // 累积 cross products
        Integer accum(1);
        uint64_t batch_start_j = j_lo;
        uint64_t steps_in_batch = 0;
        constexpr uint64_t BATCH_SIZE = 16;

        // v22: c, t buffer hoist 出 baby loop — 480 babies × ~B2/D giants
        // 在 50d 是 ~23M iterations/curve, 节省巨量 mpz_init/free
        Integer c, t;
        auto accumulate_step = [&](const Point& G) -> std::optional<Integer> {
            for (const auto& b : baby) {
                // cross = G.x * b.z - b.x * G.z (mod n)
                // c, t ∈ [0, n-1] 后 c -= t ∈ [-(n-1), n-1], 单次 if c<0: c+=n
                // 即拉回 [0, n-1] — 后续 c %= n 是 no-op, 省 mpz_mod call.
                c = G.x; c *= b.z; c %= n;
                t = b.x; t *= G.z; t %= n;
                c -= t;
                if (c.is_negative()) c += n;
                if (c.is_zero()) {
                    // c=0 means G and baby represent same point — factor may be in Z coordinate
                    Integer g = gcd(G.z, n);
                    if (!g.is_one() && g.compare(n) != 0) {
                        return g;
                    }
                } else {
                    accum *= c; accum %= n;
                }
            }
            ++steps_in_batch;
            return std::nullopt;
        };

        // 检查 gcd + 回退处理
        auto check_batch = [&](uint64_t j_current) -> std::optional<Integer> {
            if (accum.is_one()) {
                accum = int64_t(1);  // mpz_set_si direct
                batch_start_j = j_current + 1;
                steps_in_batch = 0;
                return std::nullopt;
            }
            Integer g = core::gcd(accum, n);
            if (!g.is_one() && g.compare(n) != 0) return g;
            if (g.compare(n) == 0) {
                // gcd == n: 回退到朴素实现
                uint64_t lo = (batch_start_j > 0 ? batch_start_j - 1 : 0) * D;
                if (lo < B1) lo = B1;
                uint64_t hi = std::min((j_current + 1) * D, B2);
                auto fb = stage2_naive(Q0, n, a24, lo, hi);
                if (fb) return fb;
            }
            accum = int64_t(1);  // mpz_set_si direct
            batch_start_j = j_current + 1;
            steps_in_batch = 0;
            return std::nullopt;
        };

        // 处理 j = j_lo
        if (auto f = accumulate_step(G_prev)) return f;

        if (j_lo == j_hi) {
            return check_batch(j_lo);
        }

        // 处理 j = j_lo + 1
        if (auto f = accumulate_step(G_curr)) return f;

        // 差分链: j = j_lo + 2, j_lo + 3, ...
        for (uint64_t j = j_lo + 2; j <= j_hi; ++j) {
            // 定期 gcd 检查
            if (steps_in_batch >= BATCH_SIZE) {
                auto r = check_batch(j - 1);
                if (r) return r;
            }

            // 推进链: G_next = G_curr + Q_D, diff = G_prev
            Point G_next = mont_add(G_curr, Q_D, G_prev, n);
            G_prev = std::move(G_curr);
            G_curr = std::move(G_next);

            if (auto f = accumulate_step(G_curr)) return f;
        }

        // 最终检查
        return check_batch(j_hi);
    }

    /// Stage 2 朴素实现: 逐素数 mont_mul (用于 BSGS 回退或小范围)
    [[nodiscard]] static std::optional<Integer> stage2_naive(
            const Point& Q0, const Integer& n, const Integer& a24,
            uint64_t B1, uint64_t B2) {

        Integer accum(1);
        Point Qcurr(Q0.x.clone(), Q0.z.clone());
        uint64_t check_interval = 0;
        std::optional<Integer> found;

        Point checkpoint(Q0.x.clone(), Q0.z.clone());
        std::vector<uint64_t> batch_primes;
        batch_primes.reserve(128);

        for_each_prime_in_range(B1, B2, [&](uint64_t p) -> bool {
            Qcurr = mont_mul(Qcurr, p, a24, n);
            batch_primes.push_back(p);

            accum *= Qcurr.z;
            accum %= n;

            ++check_interval;
            if (check_interval >= 100) {
                Integer g = core::gcd(accum, n);
                if (!g.is_one() && g.compare(n) != 0) {
                    found = std::move(g);
                    return false;
                }
                if (g.compare(n) == 0) {
                    Point Q_retry(checkpoint.x.clone(), checkpoint.z.clone());
                    for (uint64_t bp : batch_primes) {
                        Q_retry = mont_mul(Q_retry, bp, a24, n);
                        Integer gi = core::gcd(Q_retry.z, n);  // v22: gcd 无需 clone
                        if (!gi.is_one() && gi.compare(n) != 0) {
                            found = std::move(gi);
                            return false;
                        }
                    }
                }
                accum = int64_t(1);  // mpz_set_si direct
                checkpoint = Point(Qcurr.x.clone(), Qcurr.z.clone());
                batch_primes.clear();
                check_interval = 0;
            }
            return true;
        });

        if (found) return found;

        // v22: gcd 无需 clone
        Integer g = core::gcd(accum, n);
        if (!g.is_one() && g.compare(n) != 0) return g;
        if (g.compare(n) == 0 && !batch_primes.empty()) {
            Point Q_retry(checkpoint.x.clone(), checkpoint.z.clone());
            for (uint64_t bp : batch_primes) {
                Q_retry = mont_mul(Q_retry, bp, a24, n);
                Integer gi = core::gcd(Q_retry.z, n);  // v22
                if (!gi.is_one() && gi.compare(n) != 0) return gi;
            }
        }
        return std::nullopt;
    }
};

} // namespace gnfs::cofactor
