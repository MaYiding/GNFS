#pragma once

#include "../core/integer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <optional>
#include <span>
#include <vector>

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
    ///
    /// 实现细节: 该函数在 GNFS pipeline cofactorization Phase 4 是 hot path
    /// (smooth_check.hpp::classify_cofactor), 每个 cofactor 都调用一次。
    /// 用 thread_local cached BatchContext 跨调用复用 Stage 1 共享数据
    /// (primes_cache + prime_powers), 避免每次 alloc B1=2000 → ~300 primes
    /// + inline pk 计算。sigma 仍 per-call randomized (与 N 关联) 保留原行为。
    [[nodiscard]] static std::optional<Integer> quick_factor(const Integer& n) {
        if (n.is_one() || n.is_probable_prime() > 0) {
            return std::nullopt;
        }

        // thread_local cache: B1=2000 primes_cache + prime_powers 一次构造
        // sigma_pool 每次重新生成 (与 N 关联), 保留原 quick_factor 随机行为
        thread_local BatchContext cached_ctx = [] {
            Config cfg;
            cfg.num_curves = 0;  // sigma_pool 单独 per-call 生成, 这里不用
            cfg.B1 = 2000;
            cfg.B2 = 50000;
            cfg.auto_params = false;
            return prepare_batch(cfg, /*sigma_seed=*/0);
        }();

        // 重建 sigma_pool: 每次 quick_factor 用 N+rd seed (与原 factor 行为一致)
        std::random_device rd;
        uint64_t n_low = mpz_getlimbn(n.get_mpz(), 0);
        uint64_t seed = rd() ^ n_low;
        std::mt19937_64 rng(seed);
        cached_ctx.sigma_pool.clear();
        cached_ctx.sigma_pool.reserve(10);
        for (uint32_t i = 0; i < 10; ++i) {
            cached_ctx.sigma_pool.push_back((rng() % 1000000) + 6);
        }

        // 走 batch path: 共享 primes_cache + prime_powers
        for (uint64_t sigma : cached_ctx.sigma_pool) {
            auto result = try_curve_with_pk(n, sigma, cached_ctx);
            if (result) return result;
        }
        return std::nullopt;
    }

    /// 共享上下文 — 跨多个 cofactor 复用的 N-independent 数据
    ///
    /// 当对一批 cofactor 调用 ECM 时,以下数据与 N 无关,可预计算一次:
    ///   - primes_cache: ≤ B1 的素数表 (Stage 1)
    ///   - prime_powers: 每个 prime p 对应的 pk = max p^e ≤ B1 (Stage 1 标量)
    ///   - sigma_pool: 确定性 sigma 序列 (size = num_curves)
    ///
    /// Stage 2 BSGS baby steps (Point 数据) 含 mod N 运算,无法跨 N 共享。
    /// 真正大头的"GMP-ECM batch mode" (多 N 同时 mont_mul) 需要 SIMD 重构,
    /// 当前实现仅共享 N-independent 数据,提供 API 基础设施 + 单元测试入口。
    struct BatchContext {
        uint64_t B1 = 0;
        uint64_t B2 = 0;
        std::vector<uint64_t> primes_cache;   // ≤ B1 的素数
        std::vector<uint64_t> prime_powers;   // 与 primes_cache 等长: pk = max p^e ≤ B1
        std::vector<uint64_t> sigma_pool;     // 确定性 sigma 序列

        [[nodiscard]] bool empty() const noexcept { return primes_cache.empty(); }
        [[nodiscard]] size_t num_curves() const noexcept { return sigma_pool.size(); }
    };

    /// 构造 BatchContext (N-independent 共享数据)
    /// @param config B1/B2/num_curves 来源
    /// @param sigma_seed 0 = 用 time-based seed; 非 0 = 确定性测试可重现
    [[nodiscard]] static BatchContext prepare_batch(const Config& config,
                                                     uint64_t sigma_seed = 0) {
        BatchContext ctx;
        ctx.B1 = config.B1;
        ctx.B2 = config.B2;

        // primes_cache: 一次 sieve 复用 (相比 factor() 内 per-call)
        ctx.primes_cache = sieve_primes(config.B1);

        // prime_powers: 与 primes_cache 同序, pk = max p^e ≤ B1
        // 原 try_curve 是 inline 计算的 (`while pk ≤ B1/p`), 预计算让 N=B 个 cofactor
        // 共享 pk 序列, 节省 num_curves × N × primes_cache.size() 次内联运算
        ctx.prime_powers.reserve(ctx.primes_cache.size());
        for (uint64_t p : ctx.primes_cache) {
            uint64_t pk = p;
            while (pk <= config.B1 / p) pk *= p;  // 避免溢出
            ctx.prime_powers.push_back(pk);
        }

        // sigma_pool: 确定性序列 (sigma_seed != 0) 或 time-based (兼容现有行为)
        // 测试用 deterministic seed 让多次调用 + sequential 等价
        std::mt19937_64 rng;
        if (sigma_seed != 0) {
            rng.seed(sigma_seed);
        } else {
            std::random_device rd;
            rng.seed(rd() ^ static_cast<uint64_t>(0x9E3779B97F4A7C15ULL));
        }
        ctx.sigma_pool.reserve(config.num_curves);
        for (uint32_t i = 0; i < config.num_curves; ++i) {
            ctx.sigma_pool.push_back((rng() % 1000000) + 6);
        }

        return ctx;
    }

    /// Batch ECM: 对一批 cofactor 应用 ECM, 共享 N-independent 数据
    /// 返回 vector<optional<Integer>>, 与输入 ns 一一对应
    /// 单 cofactor 行为等价于 factor_with_batch(n, ctx)
    [[nodiscard]] static std::vector<std::optional<Integer>>
    factor_batch(std::span<const Integer> ns, const BatchContext& ctx) {
        std::vector<std::optional<Integer>> results;
        results.reserve(ns.size());
        for (const Integer& n : ns) {
            results.push_back(factor_with_batch(n, ctx));
        }
        return results;
    }

    /// 单 cofactor 使用 BatchContext 共享数据
    /// 等价于 ECM::factor() 但跳过 primes_cache + prime_powers 重建
    [[nodiscard]] static std::optional<Integer> factor_with_batch(
            const Integer& n, const BatchContext& ctx) {
        if (n.is_one() || n.is_probable_prime() > 0) {
            return std::nullopt;
        }
        if (ctx.empty()) {
            return std::nullopt;  // 防御性: 空 context 不工作
        }

        for (uint64_t sigma : ctx.sigma_pool) {
            auto result = try_curve_with_pk(n, sigma, ctx);
            if (result) {
                return result;
            }
        }
        return std::nullopt;
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

        // u = (x + z) mod n; u2 = u^2 mod n — direct GMP ops (skip mpz_set steps)
        mpz_add(u.get_mpz(), P.x.get_mpz(), P.z.get_mpz());
        u %= n;
        mpz_mul(u2.get_mpz(), u.get_mpz(), u.get_mpz());
        u2 %= n;

        // v = (x - z) mod n; v2 = v^2 mod n
        mpz_sub(v.get_mpz(), P.x.get_mpz(), P.z.get_mpz());
        if (v.is_negative()) v += n;
        v %= n;
        mpz_mul(v2.get_mpz(), v.get_mpz(), v.get_mpz());
        v2 %= n;

        // x2 = u^2 * v^2 mod n
        mpz_mul(x2.get_mpz(), u2.get_mpz(), v2.get_mpz());
        x2 %= n;

        // w = (u^2 - v^2) mod n
        mpz_sub(w.get_mpz(), u2.get_mpz(), v2.get_mpz());
        if (w.is_negative()) w += n;
        w %= n;

        // z2 = w * (v^2 + a24 * w) mod n
        mpz_mul(t.get_mpz(), a24.get_mpz(), w.get_mpz());
        t %= n;
        t += v2;
        t %= n;

        mpz_mul(z2.get_mpz(), w.get_mpz(), t.get_mpz());
        z2 %= n;

        return Point(std::move(x2), std::move(z2));
    }

    /// 蒙哥马利差分加法: P + Q (已知 P - Q)
    /// v22: thread_local workspace 复用
    static Point mont_add(const Point& P, const Point& Q, const Point& diff, const Integer& n) {
        thread_local Integer u, v, t1, t2, sum, sum2, dif, dif2;
        Integer xr, zr;  // 返回值

        // u = (Px - Pz) * (Qx + Qz) mod n — direct GMP ops
        mpz_sub(u.get_mpz(), P.x.get_mpz(), P.z.get_mpz());
        if (u.is_negative()) u += n;
        u %= n;
        mpz_add(t1.get_mpz(), Q.x.get_mpz(), Q.z.get_mpz());
        t1 %= n;
        u *= t1;
        u %= n;

        // v = (Px + Pz) * (Qx - Qz) mod n
        mpz_add(v.get_mpz(), P.x.get_mpz(), P.z.get_mpz());
        v %= n;
        mpz_sub(t2.get_mpz(), Q.x.get_mpz(), Q.z.get_mpz());
        if (t2.is_negative()) t2 += n;
        t2 %= n;
        v *= t2;
        v %= n;

        // xr = diff.z * (u + v)^2 mod n
        mpz_add(sum.get_mpz(), u.get_mpz(), v.get_mpz());
        sum %= n;
        mpz_mul(sum2.get_mpz(), sum.get_mpz(), sum.get_mpz());
        sum2 %= n;
        mpz_mul(xr.get_mpz(), diff.z.get_mpz(), sum2.get_mpz());
        xr %= n;

        // zr = diff.x * (u - v)^2 mod n
        mpz_sub(dif.get_mpz(), u.get_mpz(), v.get_mpz());
        if (dif.is_negative()) dif += n;
        dif %= n;
        mpz_mul(dif2.get_mpz(), dif.get_mpz(), dif.get_mpz());
        dif2 %= n;
        mpz_mul(zr.get_mpz(), diff.x.get_mpz(), dif2.get_mpz());
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

    /// 尝试一条曲线 — 使用 BatchContext 预计算的 prime_powers (Stage 1 标量)
    /// 与 try_curve 等价但跳过 inline `while pk ≤ B1/p` 循环, 复用 ctx.prime_powers
    [[nodiscard]] static std::optional<Integer> try_curve_with_pk(
            const Integer& n, uint64_t sigma, const BatchContext& ctx) {
        assert(sigma >= 6 && "ECM Suyama: sigma must be >= 6");
        assert(!ctx.primes_cache.empty() && "BatchContext::primes_cache must be non-empty");
        assert(ctx.prime_powers.size() == ctx.primes_cache.size()
               && "BatchContext: primes_cache and prime_powers must be parallel");

        // Suyama 参数 (与 try_curve 一致)
        Integer u(static_cast<unsigned long long>(sigma * sigma - 5));
        u %= n;

        Integer v(static_cast<unsigned long long>(4 * sigma));
        v %= n;

        Integer x0;
        mpz_powm_ui(x0.get_mpz(), u.get_mpz(), 3, n.get_mpz());

        Integer z0;
        mpz_powm_ui(z0.get_mpz(), v.get_mpz(), 3, n.get_mpz());

        Integer diff;
        mpz_sub(diff.get_mpz(), v.get_mpz(), u.get_mpz());
        if (diff.is_negative()) diff += n;
        diff %= n;

        Integer diff3;
        mpz_powm_ui(diff3.get_mpz(), diff.get_mpz(), 3, n.get_mpz());

        Integer sum3u_v;
        sum3u_v = v;
        mpz_addmul_ui(sum3u_v.get_mpz(), u.get_mpz(), 3);
        sum3u_v %= n;

        Integer numerator;
        mpz_mul(numerator.get_mpz(), diff3.get_mpz(), sum3u_v.get_mpz());
        numerator %= n;

        Integer denom;
        mpz_mul(denom.get_mpz(), x0.get_mpz(), v.get_mpz());
        denom %= n;
        mpz_mul_2exp(denom.get_mpz(), denom.get_mpz(), 4);
        denom %= n;

        Integer g = core::gcd(denom, n);
        if (!g.is_one()) {
            if (g.compare(n) == 0) return std::nullopt;
            return g;
        }

        Integer denom_inv = core::mod_inverse(denom, n);
        if (denom_inv.is_zero()) {
            return std::nullopt;
        }

        Integer a24;
        mpz_mul(a24.get_mpz(), numerator.get_mpz(), denom_inv.get_mpz());
        a24 %= n;

        Point Q(std::move(x0), std::move(z0));

        // Stage 1: 使用预计算的 prime_powers (与 primes_cache 同序)
        const size_t np = ctx.primes_cache.size();
        for (size_t i = 0; i < np; ++i) {
            uint64_t p = ctx.primes_cache[i];
            uint64_t pk = ctx.prime_powers[i];  // 预计算复用

            Q = mont_mul(Q, pk, a24, n);

            if (p % 100 == 97) {
                Integer g2 = core::gcd(Q.z, n);
                if (!g2.is_one()) {
                    if (g2.compare(n) == 0) return std::nullopt;
                    return g2;
                }
            }
        }

        Integer g_final = core::gcd(Q.z, n);
        if (!g_final.is_one()) {
            if (g_final.compare(n) == 0) return std::nullopt;
            return g_final;
        }

        // Stage 2: 复用现有 BSGS 实现 (Point 部分 N-dependent, 无法跨 N 共享)
        if (ctx.B2 > ctx.B1) {
            auto stage2_result = stage2(Q, n, a24, ctx.B1, ctx.B2);
            if (stage2_result) {
                return stage2_result;
            }
        }

        return std::nullopt;
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

        // 起始点 u^3, v^3 — mpz_powm_ui combines mul + mod
        Integer x0;
        mpz_powm_ui(x0.get_mpz(), u.get_mpz(), 3, n.get_mpz());

        Integer z0;
        mpz_powm_ui(z0.get_mpz(), v.get_mpz(), 3, n.get_mpz());

        // a24 = (v - u)^3 * (3u + v) / (16 * u^3 * v) - 2
        // 简化: 直接计算 a24 = ((v-u)^3 * (3u+v)) * inverse(16*u^3*v) - 2
        // 如果逆元不存在，我们就找到了因子!
        Integer diff;
        mpz_sub(diff.get_mpz(), v.get_mpz(), u.get_mpz());  // diff = v - u (skip clone+sub)
        if (diff.is_negative()) diff += n;
        diff %= n;

        // diff^3 mod n via mpz_powm_ui (combines mul + mod)
        Integer diff3;
        mpz_powm_ui(diff3.get_mpz(), diff.get_mpz(), 3, n.get_mpz());

        // sum3u_v = v + 3u via mpz_addmul_ui (fused FMA), then mod n
        Integer sum3u_v;
        sum3u_v = v;
        mpz_addmul_ui(sum3u_v.get_mpz(), u.get_mpz(), 3);
        sum3u_v %= n;

        // numerator = diff3 * sum3u_v mod n
        Integer numerator;
        mpz_mul(numerator.get_mpz(), diff3.get_mpz(), sum3u_v.get_mpz());
        numerator %= n;

        // denom = x0 * v * 16 mod n
        Integer denom;
        mpz_mul(denom.get_mpz(), x0.get_mpz(), v.get_mpz());
        denom %= n;
        mpz_mul_2exp(denom.get_mpz(), denom.get_mpz(), 4);  // *=16 via bit shift
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

        Integer a24;
        mpz_mul(a24.get_mpz(), numerator.get_mpz(), denom_inv.get_mpz());  // a24 = num * inv (skip clone+mul)
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

        // d=1: gcd(1, 2310) = 1 — BabyStep{Integer copy ctor}
        baby.push_back({Q0.x, Q0.z});

        for (uint64_t k = 3; k <= D; ++k) {
            Point curr = mont_add(km1, Q_one, km2, n);
            if (gcd_u64(k, D) == 1) {
                baby.push_back({curr.x, curr.z});
            }
            km2 = std::move(km1);
            km1 = std::move(curr);
        }
        // 循环结束后 km1 = D*Q (km1 not used after — move avoids GMP copy)
        Point Q_D = std::move(km1);

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
        Point Qcurr = Q0;  // copy ctor (Integer fields auto-clone)
        uint64_t check_interval = 0;
        std::optional<Integer> found;

        Point checkpoint = Q0;  // copy ctor
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
                    Point Q_retry = checkpoint;  // copy ctor
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
                checkpoint = Qcurr;  // copy assign
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
            Point Q_retry = checkpoint;  // Point copy ctor (Integer x/z have copy ctor)
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
