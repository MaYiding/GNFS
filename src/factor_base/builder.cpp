#include "gnfs/factor_base/builder.hpp"
#include "gnfs/sqrt/modular_poly.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <istream>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace gnfs::factor_base {

// ============================================================
// FactorBase methods (save/load - inline methods are in header)
// ============================================================

void FactorBase::save(std::ostream& os) const {
    // Magic + version header
    constexpr uint32_t MAGIC = 0x47464246;  // "GFBF" (GNFS Factor Base Format)
    constexpr uint32_t VERSION = 1;
    os.write(reinterpret_cast<const char*>(&MAGIC), sizeof(MAGIC));
    os.write(reinterpret_cast<const char*>(&VERSION), sizeof(VERSION));

    // Params
    os.write(reinterpret_cast<const char*>(&params_.rational_bound), sizeof(params_.rational_bound));
    os.write(reinterpret_cast<const char*>(&params_.algebraic_bound), sizeof(params_.algebraic_bound));
    os.write(reinterpret_cast<const char*>(&params_.large_prime_bound), sizeof(params_.large_prime_bound));
    os.write(reinterpret_cast<const char*>(&params_.log_scale), sizeof(params_.log_scale));

    // Sieve algebraic count
    uint64_t sac = static_cast<uint64_t>(sieve_algebraic_count_);
    os.write(reinterpret_cast<const char*>(&sac), sizeof(sac));

    // Rational primes
    uint32_t rat_count = static_cast<uint32_t>(rational_.size());
    os.write(reinterpret_cast<const char*>(&rat_count), sizeof(rat_count));
    for (const auto& rp : rational_) {
        os.write(reinterpret_cast<const char*>(&rp.p), sizeof(rp.p));
        os.write(reinterpret_cast<const char*>(&rp.log_p), sizeof(rp.log_p));
    }

    // Algebraic primes
    uint32_t alg_count = static_cast<uint32_t>(algebraic_.size());
    os.write(reinterpret_cast<const char*>(&alg_count), sizeof(alg_count));
    for (const auto& ap : algebraic_) {
        os.write(reinterpret_cast<const char*>(&ap.p), sizeof(ap.p));
        os.write(reinterpret_cast<const char*>(&ap.r), sizeof(ap.r));
        os.write(reinterpret_cast<const char*>(&ap.log_p), sizeof(ap.log_p));
        os.write(reinterpret_cast<const char*>(&ap.degree), sizeof(ap.degree));
    }
}

FactorBase FactorBase::load(std::istream& is) {
    // Magic + version
    uint32_t magic, version;
    is.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    is.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (magic != 0x47464246)
        throw std::runtime_error("FactorBase::load: invalid magic number");
    if (version != 1)
        throw std::runtime_error("FactorBase::load: unsupported version " + std::to_string(version));

    // Params
    FactorBaseParams params;
    is.read(reinterpret_cast<char*>(&params.rational_bound), sizeof(params.rational_bound));
    is.read(reinterpret_cast<char*>(&params.algebraic_bound), sizeof(params.algebraic_bound));
    is.read(reinterpret_cast<char*>(&params.large_prime_bound), sizeof(params.large_prime_bound));
    is.read(reinterpret_cast<char*>(&params.log_scale), sizeof(params.log_scale));

    FactorBase fb(params);

    // Sieve algebraic count
    uint64_t sac;
    is.read(reinterpret_cast<char*>(&sac), sizeof(sac));
    fb.sieve_algebraic_count_ = static_cast<size_t>(sac);

    // Rational primes
    uint32_t rat_count;
    is.read(reinterpret_cast<char*>(&rat_count), sizeof(rat_count));
    fb.rational_.resize(rat_count);
    for (uint32_t i = 0; i < rat_count; ++i) {
        is.read(reinterpret_cast<char*>(&fb.rational_[i].p), sizeof(uint32_t));
        is.read(reinterpret_cast<char*>(&fb.rational_[i].log_p), sizeof(uint32_t));
    }

    // Algebraic primes
    uint32_t alg_count;
    is.read(reinterpret_cast<char*>(&alg_count), sizeof(alg_count));
    fb.algebraic_.resize(alg_count);
    for (uint32_t i = 0; i < alg_count; ++i) {
        is.read(reinterpret_cast<char*>(&fb.algebraic_[i].p), sizeof(uint32_t));
        is.read(reinterpret_cast<char*>(&fb.algebraic_[i].r), sizeof(uint32_t));
        is.read(reinterpret_cast<char*>(&fb.algebraic_[i].log_p), sizeof(uint32_t));
        is.read(reinterpret_cast<char*>(&fb.algebraic_[i].degree), sizeof(uint8_t));
    }

    // Rebuild index tables
    fb.build_index();

    if (!is)
        throw std::runtime_error("FactorBase::load: unexpected end of stream");

    return fb;
}

// ============================================================
// FactorBaseBuilder methods
// ============================================================

// Note: Instance-based constructor is removed because PolynomialContext
// doesn't support copying. Use the static build method instead.

FactorBaseBuilder::FactorBaseBuilder(const PolynomialContext& /* ctx */)
    : ctx_() {
    // Cannot copy PolynomialContext, this constructor is deprecated
    // Use the static build() method instead
}

FactorBase FactorBaseBuilder::build(const PolynomialContext& ctx, const Options& opts) {
    FactorBase fb;

    // Set parameters
    uint64_t lp_bound = opts.large_prime_bound;
    if (lp_bound == 0) lp_bound = static_cast<uint64_t>(opts.rational_bound) * 100;
    core::FactorBaseParams params(
        opts.rational_bound,
        opts.algebraic_bound,
        lp_bound,
        opts.log_scale
    );
    fb.set_params(params);

    // 实际代数素数上界：含 special-Q 范围
    uint32_t effective_alg_bound = opts.algebraic_bound;
    if (opts.special_q_bound > opts.algebraic_bound) {
        effective_alg_bound = opts.special_q_bound;
    }

    // Estimate sizes for preallocation
    // Prime counting function: π(n) ≈ n / ln(n)
    // `+1` widened to uint64_t so UINT32_MAX doesn't wrap to 0 (log(0)=-inf → 0 estimate).
    size_t estimated_rational = static_cast<size_t>(
        opts.rational_bound / std::log(static_cast<double>(static_cast<uint64_t>(opts.rational_bound) + 1)) * 1.2
    );
    size_t estimated_algebraic = static_cast<size_t>(
        effective_alg_bound / std::log(static_cast<double>(static_cast<uint64_t>(effective_alg_bound) + 1)) * 1.2
    ) * ctx.degree();
    fb.reserve(estimated_rational, estimated_algebraic);

    // 构建共享的 Eratosthenes 筛,bound = max(rational, algebraic),
    // 避免两个 find_* 在 rational==algebraic(常见配置)下重复构建 12.5 MB。
    uint32_t shared_bound = std::max(opts.rational_bound, opts.algebraic_bound);
    std::vector<bool> shared_sieve = build_eratosthenes_sieve(shared_bound);

    find_rational_primes(fb, ctx, opts.rational_bound, opts.log_scale, &shared_sieve);
    find_algebraic_primes(fb, ctx, opts.algebraic_bound, opts.log_scale, &shared_sieve);

    // 记录筛选用的代数素数数量（≤ algebraic_bound 的部分）
    fb.set_sieve_algebraic_count(fb.algebraic_count());

    // 如果 special_q_bound > algebraic_bound，继续构建 SQ 范围的代数素数。
    // 注意: 必须显式过滤 algebraic_bound==UINT32_MAX 否则 +1 wrap=0 让 find_…_range
    // 收到 (min_p=0, max_p>0),min_p<2 早 return 会救住但语义不对。params.hpp 实际
    // 把 B 限到 1e9,这层是 future-proof 防御。
    if (opts.special_q_bound > opts.algebraic_bound &&
        opts.algebraic_bound < UINT32_MAX) {
        find_algebraic_primes_range(fb, ctx,
            opts.algebraic_bound + 1, opts.special_q_bound, opts.log_scale);
    }

    fb.build_index();
    return fb;
}

FactorBase FactorBaseBuilder::build(uint32_t /* rational_bound */, uint32_t /* algebraic_bound */) {
    // This instance method requires a valid ctx_, which we can't have with deleted copy.
    // The static build(ctx, opts) method should be used instead.
    throw std::logic_error(
        "FactorBaseBuilder::build(uint32_t, uint32_t) is deprecated — "
        "use the static build(ctx, opts) method instead");
}

std::vector<bool> FactorBaseBuilder::build_eratosthenes_sieve(uint32_t bound) {
    // ── 分支策略 ──
    // bound < 5e6: 单线程简单筛 (cache 命中率高,线程开销得不偿失)
    // bound ≥ 5e6: 分段并行筛 (按 L2 cache 段分,工作队列调度)
    //
    // 实测 1e7 边界: 单线程 ~100ms, 8 线程分段 ~25ms (3-4× 加速)。
    // 1e8: 单线程 ~5s, 8 线程 ~1.2s。
    constexpr uint32_t PARALLEL_THRESHOLD = 5'000'000;

    if (bound < PARALLEL_THRESHOLD) {
        std::vector<bool> is_prime(static_cast<size_t>(bound) + 1, true);
        if (bound >= 1) is_prime[0] = false;
        if (bound >= 1) is_prime[1] = false;

        for (uint64_t p = 2; p * p <= bound; ++p) {
            if (!is_prime[static_cast<size_t>(p)]) continue;
            for (uint64_t k = p * p; k <= bound; k += p) {
                is_prime[static_cast<size_t>(k)] = false;
            }
        }
        return is_prime;
    }

    // ── 分段并行筛 ──
    // Step 1: 子筛 ≤ √bound 的小素数 (单线程,数量少)
    uint32_t sqrt_bound = static_cast<uint32_t>(
        std::sqrt(static_cast<double>(bound))) + 1;
    std::vector<bool> small_sieve(static_cast<size_t>(sqrt_bound) + 1, true);
    small_sieve[0] = small_sieve[1] = false;
    for (uint64_t p = 2; p * p <= sqrt_bound; ++p) {
        if (!small_sieve[static_cast<size_t>(p)]) continue;
        for (uint64_t k = p * p; k <= sqrt_bound; k += p) {
            small_sieve[static_cast<size_t>(k)] = false;
        }
    }
    std::vector<uint32_t> small_primes;
    small_primes.reserve(static_cast<size_t>(sqrt_bound / std::log(sqrt_bound + 1.0) * 1.2));
    for (uint32_t p = 2; p <= sqrt_bound; ++p) {
        if (small_sieve[p]) small_primes.push_back(p);
    }

    // Step 2: 用 uint8_t (而非 vector<bool> 位包装) 以保证 byte 粒度
    // thread-safe writes — 不同 thread 写不同 segment 时不会 byte-share。
    std::vector<uint8_t> is_prime_u8(static_cast<size_t>(bound) + 1, 1);
    is_prime_u8[0] = is_prime_u8[1] = 0;

    // Step 3: 段并行
    // SEGMENT_SIZE 选 L2 cache (~256KB on modern CPU) 友好的大小,
    // 内层 small primes 的 inner loop 全在 cache 内。
    constexpr uint32_t SEGMENT_SIZE = 256 * 1024;
    uint32_t num_segments = (bound + SEGMENT_SIZE) / SEGMENT_SIZE;

    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) n_threads = 4;
    if (n_threads > num_segments) n_threads = num_segments;

    std::atomic<uint32_t> next_seg{0};
    auto worker = [&]() {
        while (true) {
            uint32_t s = next_seg.fetch_add(1, std::memory_order_relaxed);
            if (s >= num_segments) break;
            uint64_t seg_lo = static_cast<uint64_t>(s) * SEGMENT_SIZE;
            uint64_t seg_hi = std::min<uint64_t>(seg_lo + SEGMENT_SIZE - 1, bound);

            for (uint32_t p : small_primes) {
                uint64_t pp = static_cast<uint64_t>(p) * p;
                uint64_t start;
                if (pp >= seg_lo) {
                    start = pp;  // 第一个未划掉的倍数: p*p
                } else {
                    // ⌈seg_lo / p⌉ · p — 第一个 ≥ seg_lo 的 p 的倍数
                    start = ((seg_lo + p - 1) / p) * p;
                }
                if (start > seg_hi) continue;
                for (uint64_t k = start; k <= seg_hi; k += p) {
                    is_prime_u8[static_cast<size_t>(k)] = 0;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();

    // 转 std::vector<bool> 返回 (保持接口兼容)
    std::vector<bool> result(static_cast<size_t>(bound) + 1);
    for (size_t i = 0; i <= bound; ++i) {
        result[i] = (is_prime_u8[i] != 0);
    }
    return result;
}

void FactorBaseBuilder::find_rational_primes(FactorBase& fb, const PolynomialContext& ctx,
                                              uint32_t bound, uint8_t log_scale,
                                              const std::vector<bool>* shared_sieve) {
    // 优先用调用方传入的共享筛(build() 一次构建供 rational+algebraic 复用);
    // 否则自行构建(为了独立测试 find_rational_primes 的入口而保留)。
    std::vector<bool> local_sieve;
    const std::vector<bool>* sieve_ptr;
    if (shared_sieve != nullptr && shared_sieve->size() > static_cast<size_t>(bound)) {
        sieve_ptr = shared_sieve;
    } else {
        local_sieve = build_eratosthenes_sieve(bound);
        sieve_ptr = &local_sieve;
    }
    const auto& is_prime = *sieve_ptr;

    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;

        // Skip primes that divide N — use mpz_divisible_ui_p (zero GMP alloc)
        if (mpz_divisible_ui_p(ctx.n().get_mpz(), p)) {
            continue;
        }

        // Compute log value
        uint32_t log_p = compute_log_prime_precise(p, log_scale);

        // Add to factor base
        fb.add_rational(p, log_p);
    }
}

void FactorBaseBuilder::find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx,
                                               uint32_t bound, uint8_t log_scale,
                                               const std::vector<bool>* shared_sieve) {
    // Step 1: 复用 build() 提供的筛或自建
    std::vector<bool> local_sieve;
    const std::vector<bool>* sieve_ptr;
    if (shared_sieve != nullptr && shared_sieve->size() > static_cast<size_t>(bound)) {
        sieve_ptr = shared_sieve;
    } else {
        local_sieve = build_eratosthenes_sieve(bound);
        sieve_ptr = &local_sieve;
    }
    const auto& is_prime_sieve = *sieve_ptr;

    // Collect primes into vector (excluding N-divisors)
    // Projective root check: precompute leading_coeff mod small primes
    uint64_t fd_u64 = 0;
    bool fd_fits = ctx.leading_coeff().fits_uint64();
    if (fd_fits) fd_u64 = ctx.leading_coeff().to_uint64();

    std::vector<uint32_t> primes;
    primes.reserve(static_cast<size_t>(bound / std::log(static_cast<double>(bound + 1)) * 1.1));

    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime_sieve[p]) continue;
        if (mpz_divisible_ui_p(ctx.n().get_mpz(), p)) continue;
        primes.push_back(p);
    }

    // Step 2: Parallel root finding
    // Each thread processes a chunk of primes, collecting (p, r, log_p) entries
    struct AlgEntry {
        uint32_t p, r, log_p;
    };

    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) n_threads = 4;
    if (primes.size() < 200) n_threads = 1;

    std::vector<std::vector<AlgEntry>> thread_results(n_threads);

    auto worker = [&](size_t tid) {
        size_t chunk = primes.size() / n_threads;
        size_t start = tid * chunk;
        size_t end = (tid == n_threads - 1) ? primes.size() : start + chunk;

        auto& local = thread_results[tid];
        local.reserve((end - start) * 2); // ~2 entries per prime avg

        for (size_t i = start; i < end; ++i) {
            uint32_t p = primes[i];
            auto roots = find_roots_mod_p(ctx, p);
            uint32_t lp = compute_log_prime_precise(p, log_scale);

            for (uint32_t root : roots) {
                local.push_back({p, root, lp});
            }

            // Projective root
            bool has_proj = false;
            if (fd_fits) {
                has_proj = (fd_u64 % p == 0);
            } else {
                has_proj = mpz_divisible_ui_p(ctx.leading_coeff().get_mpz(), p) != 0;
            }
            if (has_proj) {
                local.push_back({p, core::AlgebraicPrime::PROJECTIVE_ROOT, lp});
            }
        }
    };

    if (n_threads <= 1) {
        worker(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(n_threads);
        for (size_t t = 0; t < n_threads; ++t)
            threads.emplace_back(worker, t);
        for (auto& t : threads) t.join();
    }

    // Step 3: Merge in prime order (threads process consecutive chunks, already sorted)
    for (auto& local : thread_results) {
        for (auto& e : local) {
            fb.add_algebraic(e.p, e.r, e.log_p, 1);
        }
    }
}

std::vector<uint32_t> FactorBaseBuilder::find_roots_mod_p(const PolynomialContext& ctx, uint32_t p) {
    // For very small primes, brute force is faster than the overhead of polynomial GCD
    if (p < 64) {
        std::vector<uint32_t> roots;
        roots.reserve(static_cast<size_t>(ctx.degree()));  // max d roots in F_p
        for (uint32_t r = 0; r < p; ++r) {
            if (ctx.evaluate_mod(r, p) == 0) {
                roots.push_back(r);
            }
        }
        return roots;
    }

    // Cantor-Zassenhaus algorithm: O(d^2 * log p) instead of O(p)
    // Step 1: Get f(x) mod p
    uint32_t d = ctx.degree();
    std::vector<uint64_t> f_mod(d + 1);
    for (uint32_t i = 0; i <= d; ++i) {
        core::Integer c = ctx.coeff(i).clone();
        c %= core::Integer(static_cast<int64_t>(p));
        if (c.is_negative()) c += core::Integer(static_cast<int64_t>(p));
        f_mod[i] = c.to_uint64();
    }

    // Step 2: Compute g = gcd(x^p - x, f) mod p
    // This gives the product of all distinct linear factors of f
    sqrt::ModularPoly x_poly;
    x_poly.set_coeff(1, 1);  // x

    // x^p mod f mod p
    auto x_to_p = sqrt::ModularPoly::power(x_poly, core::Integer(static_cast<int64_t>(p)), f_mod, p);

    // x^p - x mod p
    auto x_p_minus_x = sqrt::ModularPoly::sub(x_to_p, x_poly, p);

    // gcd(x^p - x, f) mod p
    sqrt::ModularPoly f_poly(f_mod);
    auto g = sqrt::ModularPoly::gcd(x_p_minus_x, f_poly, p);

    int g_deg = g.degree();
    if (g_deg <= 0) {
        // No roots
        return {};
    }

    // Step 3: Extract roots from g (the split-free part)
    // g is a product of distinct linear factors (x - r_1)(x - r_2)...
    return extract_roots_from_poly(g, f_mod, p);
}

/// Extract roots from a polynomial that is a product of distinct linear factors mod p
/// Uses Cantor-Zassenhaus splitting when degree > 1
std::vector<uint32_t> FactorBaseBuilder::extract_roots_from_poly(
        const sqrt::ModularPoly& poly, const std::vector<uint64_t>& f_mod, uint32_t p) {

    int deg = poly.degree();
    if (deg <= 0) return {};

    if (deg == 1) {
        // Linear: ax + b = 0 → x = -b/a mod p
        uint64_t a = poly.coeff(1);
        uint64_t b = poly.coeff(0);
        // x = -b * a^{-1} mod p
        // a^{-1} = a^{p-2} mod p (Fermat)
        uint64_t a_inv = 1;
        {
            uint64_t base = a % p, exp = p - 2;
            uint64_t result = 1;
            while (exp > 0) {
                if (exp & 1) result = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(result) * base) % p);
                base = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(base) * base) % p);
                exp >>= 1;
            }
            a_inv = result;
        }
        uint64_t root = static_cast<uint64_t>(
            (static_cast<__uint128_t>(p - b) * a_inv) % p);
        return {static_cast<uint32_t>(root)};
    }

    // Degree > 1: use Cantor-Zassenhaus random splitting
    std::mt19937_64 rng(p);  // deterministic seed per prime
    std::vector<uint32_t> roots;

    // Try random splits
    for (int attempt = 0; attempt < 100 && static_cast<int>(roots.size()) < deg; ++attempt) {
        uint64_t a = rng() % p;

        // Compute gcd(poly, (x + a)^{(p-1)/2} - 1) mod p
        // This splits poly roughly in half (probabilistically)
        sqrt::ModularPoly x_plus_a;
        x_plus_a.set_coeff(0, a);
        x_plus_a.set_coeff(1, 1);

        // Get coefficients of current poly for reduction
        std::vector<uint64_t> poly_coeffs;
        poly_coeffs.reserve(static_cast<size_t>(poly.degree() + 1));
        for (int i = 0; i <= poly.degree(); ++i) {
            poly_coeffs.push_back(poly.coeff(i));
        }

        // (x+a)^{(p-1)/2} mod poly mod p
        core::Integer exp_val(static_cast<int64_t>((p - 1) / 2));
        auto power_result = sqrt::ModularPoly::power(x_plus_a, exp_val, poly_coeffs, p);

        // Subtract 1
        uint64_t c0 = power_result.coeff(0);
        c0 = (c0 + p - 1) % p;
        power_result.set_coeff(0, c0);

        // gcd with poly
        auto factor = sqrt::ModularPoly::gcd(power_result, poly, p);

        int factor_deg = factor.degree();
        if (factor_deg > 0 && factor_deg < deg) {
            // Successfully split! Recurse on both factors
            auto roots1 = extract_roots_from_poly(factor, f_mod, p);

            // Compute the other factor: poly / factor mod p
            // Simple polynomial division since factor divides poly exactly
            auto other = poly_div_mod(poly, factor, p);
            auto roots2 = extract_roots_from_poly(other, f_mod, p);

            // Reserve total before merging to avoid 2-step realloc
            roots.reserve(roots.size() + roots1.size() + roots2.size());
            roots.insert(roots.end(), roots1.begin(), roots1.end());
            roots.insert(roots.end(), roots2.begin(), roots2.end());
            return roots;
        }
    }

    // Fallback: brute force for this specific polynomial (shouldn't happen often)
    for (uint32_t r = 0; r < p && static_cast<int>(roots.size()) < deg; ++r) {
        uint64_t val = 0, rp = 1;
        for (int i = 0; i <= poly.degree(); ++i) {
            val = (val + static_cast<uint64_t>(
                (static_cast<__uint128_t>(poly.coeff(i)) * rp) % p)) % p;
            rp = static_cast<uint64_t>((static_cast<__uint128_t>(rp) * r) % p);
        }
        if (val == 0) roots.push_back(r);
    }
    return roots;
}

/// Polynomial division: compute a / b mod p (exact division)
sqrt::ModularPoly FactorBaseBuilder::poly_div_mod(
        const sqrt::ModularPoly& a, const sqrt::ModularPoly& b, uint32_t p) {

    if (b.degree() < 0 || (b.degree() == 0 && b.coeff(0) == 0)) {
        return sqrt::ModularPoly();  // division by zero
    }

    // Copy a's coefficients
    std::vector<uint64_t> rem;
    for (int i = 0; i <= a.degree(); ++i) {
        rem.push_back(a.coeff(i));
    }

    int a_deg = a.degree();
    int b_deg = b.degree();
    int q_deg = a_deg - b_deg;
    if (q_deg < 0) return sqrt::ModularPoly();

    // Inverse of leading coefficient of b
    uint64_t b_lead = b.coeff(b_deg);
    uint64_t b_lead_inv = 1;
    {
        uint64_t base = b_lead % p, exp = p - 2, result = 1;
        while (exp > 0) {
            if (exp & 1) result = static_cast<uint64_t>(
                (static_cast<__uint128_t>(result) * base) % p);
            base = static_cast<uint64_t>(
                (static_cast<__uint128_t>(base) * base) % p);
            exp >>= 1;
        }
        b_lead_inv = result;
    }

    std::vector<uint64_t> quotient(q_deg + 1, 0);

    for (int i = q_deg; i >= 0; --i) {
        uint64_t coeff = static_cast<uint64_t>(
            (static_cast<__uint128_t>(rem[i + b_deg]) * b_lead_inv) % p);
        quotient[i] = coeff;

        for (int j = 0; j <= b_deg; ++j) {
            uint64_t sub = static_cast<uint64_t>(
                (static_cast<__uint128_t>(coeff) * b.coeff(j)) % p);
            rem[i + j] = (rem[i + j] + p - sub) % p;
        }
    }

    return sqrt::ModularPoly(quotient);
}

void FactorBaseBuilder::find_algebraic_primes_range(FactorBase& fb, const PolynomialContext& ctx,
                                                     uint32_t min_p, uint32_t max_p, uint8_t log_scale) {
    if (min_p > max_p || min_p < 2) return;

    // Step 1: Sieve primes in [min_p, max_p](借公共 helper,统一起 p*p 优化)
    std::vector<bool> is_prime_sieve = build_eratosthenes_sieve(max_p);

    // Collect primes in range
    bool fd_fits = ctx.leading_coeff().fits_uint64();
    uint64_t fd_u64 = fd_fits ? ctx.leading_coeff().to_uint64() : 0;

    std::vector<uint32_t> primes;
    for (uint64_t p = min_p; p <= max_p; ++p) {
        if (!is_prime_sieve[static_cast<size_t>(p)]) continue;
        uint32_t p32 = static_cast<uint32_t>(p);
        if (mpz_divisible_ui_p(ctx.n().get_mpz(), p32)) continue;
        primes.push_back(p32);
    }

    // Step 2: Parallel root finding
    struct AlgEntry {
        uint32_t p, r, log_p;
    };

    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) n_threads = 4;
    if (primes.size() < 200) n_threads = 1;

    std::vector<std::vector<AlgEntry>> thread_results(n_threads);

    auto worker = [&](size_t tid) {
        size_t chunk = primes.size() / n_threads;
        size_t start = tid * chunk;
        size_t end = (tid == n_threads - 1) ? primes.size() : start + chunk;

        auto& local = thread_results[tid];
        local.reserve((end - start) * 2);

        for (size_t i = start; i < end; ++i) {
            uint32_t p = primes[i];
            auto roots = find_roots_mod_p(ctx, p);
            uint32_t lp = compute_log_prime_precise(p, log_scale);

            for (uint32_t root : roots) {
                local.push_back({p, root, lp});
            }

            bool has_proj = fd_fits ? (fd_u64 % p == 0)
                                    : (mpz_divisible_ui_p(ctx.leading_coeff().get_mpz(), p) != 0);
            if (has_proj) {
                local.push_back({p, core::AlgebraicPrime::PROJECTIVE_ROOT, lp});
            }
        }
    };

    if (n_threads <= 1) {
        worker(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(n_threads);
        for (size_t t = 0; t < n_threads; ++t)
            threads.emplace_back(worker, t);
        for (auto& t : threads) t.join();
    }

    // Step 3: Merge (thread chunks are consecutive, preserving prime order)
    for (auto& local : thread_results) {
        for (auto& e : local) {
            fb.add_algebraic(e.p, e.r, e.log_p, 1);
        }
    }
}

} // namespace gnfs::factor_base
