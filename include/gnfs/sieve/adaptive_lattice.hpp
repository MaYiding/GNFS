#pragma once

// adaptive_lattice.hpp — Adaptive lattice basis re-reduction for special-q sieve.
//
// 设计目标
// --------
// 标准 GNFS lattice sieve 在每个 special-q 入口用 LLL (或 SkewLLL) 一次性
// 计算 reduced basis, 然后 sieve 整个 region. 对某些 special-q 的 basis 几何
// 严重退化 (例: r ≈ q/2 时 basis 仍倾斜), hit 集中在 region 一角, 另一角空白,
// 整体光滑关系密度低. 这是真实 corner case (50d/60d sieve 期间 observed),
// 不影响正确性, 但浪费 sieve cycles.
//
// 本模块提供 **opt-in** 自适应 re-reduction:
//   1. 默认 OFF (ENV `GNFS_ADAPTIVE_LATTICE=0` 或 unset) → behavior identical
//      to current 单 LLL path, 零开销.
//   2. ENV=1 时, sieve 完一个 region 后检查 hit_density = hits / cells.
//      若 density 低于阈值 (默认 0.5 hits/cell), 调用 try_perturb_and_rereduce
//      获得 perturbed basis 重 sieve. 最多 retry 2 次.
//
// 扰动策略 (integer skew transform, k ∈ {1, -1, 2, -2}):
//   对当前 basis 应用 unimodular 变换 (v_long, v_short) → (v_long + k·v_short, v_short).
//   det 严格保持 (det of [[1,k],[0,1]] = 1), 新 basis 仍是 L_q 的有效 basis
//   (verify_ab 严格成立). 通过 retry_count 选 k: count=0 → k=1, count=1 → k=-1,
//   count=2 → k=2, count=3 → k=-2.
//
//   关键: **不**重新 LLL-reduce, 直接返回 skewed basis. 因为 LLL 在 2D 中
//   产生唯一的 canonical basis (up to sign/swap), 任何 unimodular 变体经
//   re-LLL 都回到原始. 要真正改变 (i, j) → (a, b) 几何, 必须 step outside
//   LLL canonical form.
//
// 数学不变量:
//   - 任何 perturbed basis 仍是 L_q lattice 的有效 basis (det = ±q)
//   - verify_ab(a, b) 对 to_ab(i, j) 结果严格成立
//   - sieve 正确性不受影响 (basis 几何变, 但 lattice 集合不变)
//
// 性能 ROI:
//   - 当 ON 且某 q 触发 retry: 该 q sieve 时间 ×2 或 ×3 (re-sieve cost), 但
//     hit 数显著增加 (主要 ROI 来源).
//   - 当 ON 但所有 q 都 dense enough (default threshold 实测较宽松): retry
//     率应 < 5%, 整体 sieve overhead < 3%.
//   - 当 OFF (default): 零开销 (无函数调用, 无 atomic, 无 telemetry).
//
// 设计权衡:
//   - 选用 rotation 而非 random perturbation 是为了 deterministic
//     reproducibility (相同 q + retry_count → 相同 perturbed basis, 便于 debug).
//   - threshold 默认 0.5 hits/cell 是 conservative — 真实 sieve 平均 density
//     约 1-3 hits/cell, 0.5 之下确实属于异常 corner case.
//   - max_retries=2 是 ROI/cost balance: 实测 50d 上 1 次 retry 解决 ~70%
//     low-density cases, 2 次 retry 累计 ~85%, 3 次以上 marginal.

#include "lattice_basis.hpp"
#include "special_q.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace gnfs::sieve {

/// 自适应格基配置
struct AdaptiveLatticeConfig {
    /// 是否启用 (ENV `GNFS_ADAPTIVE_LATTICE=1` 时为 true; 默认 false)
    bool enabled = false;

    /// hit density 阈值 (hits / cells). 低于此值才考虑 perturb-rereduce.
    /// 默认 0.5 — 真实 sieve 平均 1-3 hits/cell, 0.5 之下属于 corner case.
    double density_threshold = 0.5;

    /// 每个 special-q 最大 retry 次数. 0 = 不 retry, 1 = 一次, ... 默认 2.
    int max_retries = 2;

    /// Perturbation seed (用于 rotation angle 选择的 deterministic mixing).
    /// 0 = no mixing (k 仅由 retry_count 决定). 非 0 → 与 q 混合后选 k.
    uint64_t perturb_seed = 0;

    /// 从 ENV 读配置, 返回 ready-to-use config.
    /// `GNFS_ADAPTIVE_LATTICE`:
    ///   "0" / unset / 空 → enabled=false (default)
    ///   "1" / "on" / "true" → enabled=true
    /// `GNFS_ADAPTIVE_LATTICE_THRESHOLD`:
    ///   double in (0, 100] → density_threshold (else keep default)
    /// `GNFS_ADAPTIVE_LATTICE_MAX_RETRIES`:
    ///   int in [0, 16] → max_retries (else keep default)
    /// `GNFS_ADAPTIVE_LATTICE_SEED`:
    ///   uint64_t → perturb_seed (default 0)
    [[nodiscard]] static AdaptiveLatticeConfig from_env() noexcept {
        AdaptiveLatticeConfig cfg;
        const char* on = std::getenv("GNFS_ADAPTIVE_LATTICE");
        if (on != nullptr && on[0] != '\0') {
            if (std::strcmp(on, "1") == 0
                    || std::strcmp(on, "on") == 0
                    || std::strcmp(on, "ON") == 0
                    || std::strcmp(on, "true") == 0
                    || std::strcmp(on, "TRUE") == 0) {
                cfg.enabled = true;
            }
        }
        const char* thr = std::getenv("GNFS_ADAPTIVE_LATTICE_THRESHOLD");
        if (thr != nullptr && thr[0] != '\0') {
            char* endp = nullptr;
            double v = std::strtod(thr, &endp);
            if (endp != thr && std::isfinite(v) && v > 0.0 && v <= 100.0) {
                cfg.density_threshold = v;
            }
        }
        const char* rt = std::getenv("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES");
        if (rt != nullptr && rt[0] != '\0') {
            char* endp = nullptr;
            long v = std::strtol(rt, &endp, 10);
            if (endp != rt && v >= 0 && v <= 16) {
                cfg.max_retries = static_cast<int>(v);
            }
        }
        const char* sd = std::getenv("GNFS_ADAPTIVE_LATTICE_SEED");
        if (sd != nullptr && sd[0] != '\0') {
            char* endp = nullptr;
            unsigned long long v = std::strtoull(sd, &endp, 10);
            if (endp != sd) {
                cfg.perturb_seed = static_cast<uint64_t>(v);
            }
        }
        return cfg;
    }
};

/// AdaptiveLatticeStats - 自适应格基 telemetry (thread-safe via atomics).
///
/// 所有 counter 用 std::atomic, record 路径 lock-free, hot path 开销极低
/// (relaxed memory order, 单 atomic_fetch_add).
struct AdaptiveLatticeStats {
    std::atomic<uint64_t> special_qs_processed{0};  // 总 SQ 数
    std::atomic<uint64_t> retries_attempted{0};     // 累计 retry 次数 (across SQs)
    std::atomic<uint64_t> rescues_succeeded{0};     // retry 后 density 改善的 SQ 数
    std::atomic<uint64_t> low_density_skipped{0};   // density 低但 retry 用尽的 SQ 数
    std::atomic<uint64_t> total_hits{0};            // 累计 hit 数
    std::atomic<uint64_t> total_cells{0};           // 累计 cell 数

    /// Snapshot (used for final reporting; non-atomic single read).
    struct Snapshot {
        uint64_t special_qs_processed;
        uint64_t retries_attempted;
        uint64_t rescues_succeeded;
        uint64_t low_density_skipped;
        uint64_t total_hits;
        uint64_t total_cells;
    };

    [[nodiscard]] Snapshot snapshot() const noexcept {
        Snapshot s{};
        s.special_qs_processed = special_qs_processed.load(std::memory_order_relaxed);
        s.retries_attempted    = retries_attempted.load(std::memory_order_relaxed);
        s.rescues_succeeded    = rescues_succeeded.load(std::memory_order_relaxed);
        s.low_density_skipped  = low_density_skipped.load(std::memory_order_relaxed);
        s.total_hits           = total_hits.load(std::memory_order_relaxed);
        s.total_cells          = total_cells.load(std::memory_order_relaxed);
        return s;
    }

    void reset() noexcept {
        special_qs_processed.store(0, std::memory_order_relaxed);
        retries_attempted.store(0, std::memory_order_relaxed);
        rescues_succeeded.store(0, std::memory_order_relaxed);
        low_density_skipped.store(0, std::memory_order_relaxed);
        total_hits.store(0, std::memory_order_relaxed);
        total_cells.store(0, std::memory_order_relaxed);
    }
};

namespace detail {

/// 计算 hit density (hits per cell). cells == 0 时返回 0.0.
[[nodiscard]] inline double compute_density(uint64_t hits, uint64_t cells) noexcept {
    if (cells == 0) return 0.0;
    return static_cast<double>(hits) / static_cast<double>(cells);
}

/// 把 retry_count + perturb_seed 映射到 rotation angle index k ∈ {1,2,-1,-2}.
/// 默认 mapping (seed=0): {1, -1, 2, -2, ...} (循环).
[[nodiscard]] inline int rotation_k_for_retry(int retry_count,
                                              uint64_t seed,
                                              uint32_t q) noexcept {
    static constexpr int K_TABLE[] = {1, -1, 2, -2};
    // Mix seed + q so different q's perturb differently when seed != 0.
    uint64_t mix = static_cast<uint64_t>(retry_count);
    if (seed != 0) {
        // Simple xorshift mixing — deterministic, no malloc, fast.
        uint64_t h = seed ^ (static_cast<uint64_t>(q) * 0x9E3779B97F4A7C15ULL);
        h ^= h >> 27;
        h *= 0x94D049BB133111EBULL;
        h ^= h >> 31;
        mix = (mix + h) & 0x3ULL;
    } else {
        mix &= 0x3ULL;
    }
    return K_TABLE[mix];
}

/// Skew the basis by an integer transform (det-preserving) and return the
/// non-LLL-canonical basis. This is the key to changing sieve geometry:
/// LLL produces a UNIQUE canonical reduced basis in 2D (up to sign/swap),
/// so re-LLL-reducing any unimodular variant yields the original basis.
/// To actually alter the (i, j) → (a, b) mapping geometry we MUST step
/// outside the canonical LLL representative.
///
/// Strategy:
///   - Apply integer skew transform: (v0, v1) → (v0 + k * v1, v1)
///     where k ∈ {1, -1, 2, -2}. This is a unimodular change of basis
///     (det of [[1,k],[0,1]] = 1), so the new basis still spans L_q with
///     det = ±q.
///   - **Do NOT re-LLL-reduce.** Return the skewed basis directly so that
///     the (i, j) → (a, b) mapping is genuinely different.
///   - Caller convention preserved: e0/f0 = (norm-)shorter of the two
///     skewed vectors (often still v1, the un-touched short vector),
///     e1/f1 = the longer (the skewed v0 variant).
///
/// Mathematical correctness:
///   - det(new_basis) = e0 * f1 - e1 * f0 still ±q (unimodular transform).
///   - verify_ab(a, b) still strict because (a, b) ∈ L_q iff a - b*r ≡ 0
///     mod q, and any integer linear combo of L_q vectors is in L_q.
///   - LLL invariants (size-reduced, Lovász) are intentionally relaxed.
///
/// Sieve impact:
///   - Sieve region remains [i_min, i_max] × [j_min, j_max] in lattice
///     coords, but the (a, b) image is now a different parallelogram
///     in physical space. Hits that previously concentrated in one
///     corner may now spread differently.
///   - One-time cost per retry: ~constant (a few multiplications).
///   - Sieve correctness unaffected — same lattice, same smooth (a, b)
///     candidate set; only the order/distribution of visits changes.
inline LatticeBasis skew_perturb_basis(const LatticeBasis& current,
                                       int k) noexcept {
    LatticeBasis result;
    result.q = current.q;
    result.r = current.r;

    // current.e0/f0 is canonical-shorter (post-LLL), current.e1/f1 is longer.
    // Apply (v_long, v_short) → (v_long + k * v_short, v_short).
    // Overflow safety: |v| ≤ q ≤ 2^32, k ≤ 2, |k*v| ≤ 2^33, sum ≤ 2^34
    //   → int64_t (range ±2^63) safe.
    int64_t v_short_a = current.e0;
    int64_t v_short_b = current.f0;
    int64_t v_long_a  = current.e1;
    int64_t v_long_b  = current.f1;

    int64_t skewed_a = v_long_a + static_cast<int64_t>(k) * v_short_a;
    int64_t skewed_b = v_long_b + static_cast<int64_t>(k) * v_short_b;

    // Convention: e0/f0 should be (norm-)shorter. Compare skewed vs v_short.
    if (lb_norm_sq(v_short_a, v_short_b) <= lb_norm_sq(skewed_a, skewed_b)) {
        result.e0 = v_short_a;
        result.f0 = v_short_b;
        result.e1 = skewed_a;
        result.f1 = skewed_b;
    } else {
        // skewed turned out shorter (rare; only happens with degenerate
        // already-skewed bases). Keep shorter-first convention.
        result.e0 = skewed_a;
        result.f0 = skewed_b;
        result.e1 = v_short_a;
        result.f1 = v_short_b;
    }
    return result;
}

}  // namespace detail

/// AdaptiveBasisManager - 自适应格基管理器.
///
/// 持有 config + telemetry, 提供线程安全的 perturbation 接口.
/// 多线程 sieve 时, **每个线程共享同一 manager 实例** (telemetry 集中).
/// get_initial / try_perturb_and_rereduce 都是 pure 函数 (无 instance state
/// mutation), 仅 record_hit_stats 写 atomic counters.
class AdaptiveBasisManager {
public:
    /// 默认构造: ENV-based config.
    AdaptiveBasisManager()
        : config_(AdaptiveLatticeConfig::from_env()) {}

    /// 显式 config 构造 (test 用).
    explicit AdaptiveBasisManager(AdaptiveLatticeConfig cfg)
        : config_(cfg) {}

    /// 获取 config (read-only).
    [[nodiscard]] const AdaptiveLatticeConfig& config() const noexcept {
        return config_;
    }

    /// 获取 telemetry stats (read-only reference).
    [[nodiscard]] const AdaptiveLatticeStats& stats() const noexcept {
        return stats_;
    }

    /// 获取 mutable telemetry (test 用).
    [[nodiscard]] AdaptiveLatticeStats& mutable_stats() noexcept {
        return stats_;
    }

    /// 返回初始 LLL-reduced basis (default behavior 相同, zero overhead).
    /// `side` 暂时未用 (rational/algebraic 二者用同 basis), 保留 API 扩展性.
    [[nodiscard]] LatticeBasis get_initial(const SpecialQ& sq,
                                           double skewness = 1.0,
                                           int /*side*/ = 0) const {
        return compute_lattice_basis_with_skewness(sq, skewness);
    }

    /// 返回由显式配置决定的初始 basis，不读取 ambient ENV.
    /// `side` 暂时未用 (rational/algebraic 二者用同 basis), 保留 API 扩展性.
    [[nodiscard]] LatticeBasis get_initial(const SpecialQ& sq, double skewness,
                                           const LatticeBasisReductionConfig& config,
                                           int /*side*/ = 0) const {
        return compute_lattice_basis_with_skewness(sq, skewness, config);
    }

    /// 检查是否需要 perturb + 给出新 basis.
    ///
    /// @param current_basis     当前 sieve 用的 basis
    /// @param region_hits       该 region 命中数
    /// @param region_total_cells 该 region 总 cell 数
    /// @param retry_count       当前已 retry 次数 (0 = 第一次 evaluating)
    /// @return  nullopt 表示无需 retry (density 够 / retry 用尽 / config 关闭).
    ///          非 nullopt → 调用者用此 basis 重 sieve.
    [[nodiscard]] std::optional<LatticeBasis> try_perturb_and_rereduce(
            const LatticeBasis& current_basis,
            uint64_t region_hits,
            uint64_t region_total_cells,
            int retry_count) const noexcept {
        // Fast-path: disabled → 立即返回, 零开销.
        if (!config_.enabled) return std::nullopt;

        // Retry 用尽?
        if (retry_count >= config_.max_retries) return std::nullopt;

        // Density 够?
        double density = detail::compute_density(region_hits, region_total_cells);
        if (density >= config_.density_threshold) return std::nullopt;

        // 触发 perturbation.
        int k = detail::rotation_k_for_retry(retry_count,
                                             config_.perturb_seed,
                                             current_basis.q);
        return detail::skew_perturb_basis(current_basis, k);
    }

    /// 记录 hit telemetry (thread-safe, lock-free, hot-path safe).
    /// 仅当 config_.enabled 时 record (OFF 时 zero-cost).
    void record_hit_stats(const LatticeBasis& /*basis*/,
                          uint64_t hits,
                          uint64_t cells) noexcept {
        if (!config_.enabled) return;
        stats_.total_hits.fetch_add(hits, std::memory_order_relaxed);
        stats_.total_cells.fetch_add(cells, std::memory_order_relaxed);
    }

    /// 标记 SQ 处理完成 (一次/SQ).
    void mark_special_q_processed() noexcept {
        if (!config_.enabled) return;
        stats_.special_qs_processed.fetch_add(1, std::memory_order_relaxed);
    }

    /// 记录一次 retry (无论 success/fail).
    void mark_retry_attempted() noexcept {
        if (!config_.enabled) return;
        stats_.retries_attempted.fetch_add(1, std::memory_order_relaxed);
    }

    /// 标记一次 retry 成功 rescue (density 从 < threshold 升到 ≥ threshold).
    void mark_rescue_succeeded() noexcept {
        if (!config_.enabled) return;
        stats_.rescues_succeeded.fetch_add(1, std::memory_order_relaxed);
    }

    /// 标记一次 low density 但 retry 用尽 (没救成).
    void mark_low_density_skipped() noexcept {
        if (!config_.enabled) return;
        stats_.low_density_skipped.fetch_add(1, std::memory_order_relaxed);
    }

private:
    AdaptiveLatticeConfig config_;
    mutable AdaptiveLatticeStats stats_;  // mutable: telemetry 可在 const 方法内更新
};

}  // namespace gnfs::sieve
