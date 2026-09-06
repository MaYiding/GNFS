#pragma once

#include "../core/polynomial_context.hpp"
#include "../core/relation.hpp"
#include "../core/types.hpp"
#include "../factor_base/factor_base.hpp"
#include "../util/joined_worker_group.hpp"
#include "../util/joining_thread.hpp"
#include "../util/primes.hpp"
#include "../util/safe_math.hpp"
#include "adaptive_lattice.hpp"
#include "bucket_prefetch.hpp"
#include "ecore_qos.hpp"
#include "lattice_basis.hpp"
#include "special_q.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace gnfs::sieve {

namespace detail {

/// Apply constant log_p to a contiguous uint16_t range: arr[i] += lp for all i.
/// Used by Phase 0 global hits + v-prime full-row sieve. NEON 8-lane on arm64,
/// scalar elsewhere.
///
/// P2-A baseline (BACKLOG): explicit sieve-kernel NEON. doctrine 说 these
/// patterns (constant broadcast across row/area) are sieve's most
/// vectorize-friendly segments. bucket-entry scatter + tiny stride were
/// previously kept scalar because gather/strided patterns are not native NEON
/// strengths.  The tiny prime helpers below extend the baseline to that
/// territory via fixed-stride manual unrolling.
inline void apply_log_p_range(uint16_t* arr, size_t len, uint16_t lp) {
#ifdef __ARM_NEON
    size_t i = 0;
    const uint16x8_t lp_vec = vdupq_n_u16(lp);
    const size_t vec_end = len & ~size_t(7);
    for (; i < vec_end; i += 8) {
        uint16x8_t v = vld1q_u16(arr + i);
        v = vaddq_u16(v, lp_vec);
        vst1q_u16(arr + i, v);
    }
    for (; i < len; ++i)
        arr[i] += lp;
#else
    for (size_t i = 0; i < len; ++i)
        arr[i] += lp;
#endif
}

/// Returns true if the tiny-prime SIMD fast paths should run.
/// Disabled by setting `GNFS_SIEVE_NO_TINY_SIMD=1` for benchmark / debug.
/// The lookup is cached after the first call so the fast path stays branchless
/// in inner loops.
inline bool tiny_simd_enabled() noexcept {
    static const bool enabled = []() {
        const char* env = std::getenv("GNFS_SIEVE_NO_TINY_SIMD");
        if (env == nullptr)
            return true;
        if (env[0] == '\0')
            return true;
        // Treat "0" / empty as still enabled; any other value disables.
        if (env[0] == '0' && env[1] == '\0')
            return true;
        return false;
    }();
    return enabled;
}

/// Apply constant log_p at positions {start, start+stride, start+2*stride, ...}
/// while the index stays in [0, end).  Mirrors the scalar:
///
///     for (size_t idx = start; idx < end; idx += stride)
///         arr[idx] += lp;
///
/// The manual 4x unroll keeps independent dependency chains so the M-series
/// core schedules four pipelined loads/adds/stores per iteration even though
/// the writes are not contiguous (NEON has no scatter store on ARMv8.0).
/// When `GNFS_SIEVE_NO_TINY_SIMD=1` the function delegates to the scalar
/// reference for byte-for-byte parity validation.
inline void apply_log_p_stride(uint16_t* arr, size_t start, size_t end, size_t stride, uint16_t lp,
                               bool enable_tiny_simd) noexcept {
    if (start >= end || stride == 0)
        return;

    if (!enable_tiny_simd) {
        size_t idx = start;
        while (true) {
            arr[idx] += lp;
            // A guarded increment prevents wraparound when the helper is
            // called with an end point near SIZE_MAX.
            if (stride >= end - idx)
                break;
            idx += stride;
        }
        return;
    }

    // The 4x body below intentionally multiplies stride.  Route only the
    // otherwise-unrepresentable multiplication range through the guarded
    // scalar loop; normal sieve primes stay on the original fast path.
    if (stride > std::numeric_limits<size_t>::max() / 4) {
        size_t idx = start;
        while (true) {
            arr[idx] = static_cast<uint16_t>(arr[idx] + lp);
            if (stride >= end - idx)
                break;
            idx += stride;
        }
        return;
    }

    size_t idx = start;
    const size_t step4 = stride * 4;
    const size_t step3 = stride * 3;
    // Stay 3 strides shy of the end so the unrolled body never overshoots.
    // Use subtraction rather than `idx + step3` to keep the comparison
    // defined when end itself is SIZE_MAX.
    while (end - idx > step3) {
        // 4 independent dependency chains — the OoO pipeline overlaps them.
        const size_t i0 = idx;
        const size_t i1 = idx + stride;
        const size_t i2 = idx + 2 * stride;
        const size_t i3 = idx + 3 * stride;
        arr[i0] = static_cast<uint16_t>(arr[i0] + lp);
        arr[i1] = static_cast<uint16_t>(arr[i1] + lp);
        arr[i2] = static_cast<uint16_t>(arr[i2] + lp);
        arr[i3] = static_cast<uint16_t>(arr[i3] + lp);
        // If fewer than five writes remain, the unrolled body just completed
        // the range. Otherwise this update is proven to stay below `end`.
        if (end - idx <= step4)
            return;
        idx += step4;
    }

    // Tail (0-3 remaining writes), with the same guarded increment as the
    // scalar path.  The condition is checked after each write so no final
    // one-past value needs to be representable.
    while (true) {
        arr[idx] = static_cast<uint16_t>(arr[idx] + lp);
        if (stride >= end - idx)
            break;
        idx += stride;
    }
}

/// Legacy overload retaining the ambient ENV gate.
inline void apply_log_p_stride(uint16_t* arr, size_t start, size_t end, size_t stride,
                               uint16_t lp) noexcept {
    apply_log_p_stride(arr, start, end, stride, lp, tiny_simd_enabled());
}

/// Scalar reference for `apply_log_p_stride` — exposed for parity tests.
inline void apply_log_p_stride_scalar(uint16_t* arr, size_t start, size_t end, size_t stride,
                                      uint16_t lp) noexcept {
    if (stride == 0)
        return;
    if (start >= end)
        return;

    size_t idx = start;
    while (true) {
        arr[idx] = static_cast<uint16_t>(arr[idx] + lp);
        if (stride >= end - idx)
            break;
        idx += stride;
    }
}

} // namespace detail

using core::ABPair;
using core::PolynomialContext;
using core::Relation;
using factor_base::FactorBase;

/// 筛法参数
struct SieveParams {
    uint8_t log_scale = core::SIEVE_LOG_SCALE; // log 值缩放因子
    uint16_t rational_threshold = 70;          // 有理侧阈值 (uint16_t: LP 模式需 >255)
    uint16_t algebraic_threshold = 70;         // 代数侧阈值 (uint16_t: LP 模式需 >255)
    uint32_t large_prime_bound = 0;            // 大素数上界（0 = 使用因子基设置）
    bool enable_2lp = true;                    // 启用 2LP (two large primes)
    bool enable_3lp = false;                   // 启用 3LP (three large primes)

    /// 计算合并阈值
    [[nodiscard]] uint16_t combined_threshold() const noexcept {
        uint32_t sum = static_cast<uint32_t>(rational_threshold) + algebraic_threshold;
        return static_cast<uint16_t>(std::min(sum, static_cast<uint32_t>(UINT16_MAX)));
    }
};

/// Explicit execution policy for deterministic lattice-sieve execution.
///
/// Supplying this config prevents lattice basis selection, adaptive retries,
/// thread-count fallback, E-core selection, tiny-prime SIMD, and bucket
/// prefetch from consulting ENV or host hardware state. The legacy constructor
/// retains the historical ambient behavior.
struct LatticeSieveExecutionConfig {
    LatticeBasisReductionConfig lattice_basis;
    AdaptiveLatticeConfig adaptive_lattice;
    /// Used only when neither the call nor set_max_threads() supplied a count.
    /// Zero is normalized to one instead of consulting hardware_concurrency().
    size_t fallback_thread_count = 1;
    /// Requested E-core/QoS worker count, clamped to thread_count - 1.
    size_t ecore_thread_count = 0;
    bool enable_tiny_simd = true;
    bool enable_bucket_prefetch = bucket_prefetch_supported();
};

/// 筛候选点
struct SieveCandidate {
    int32_t i;        // 格坐标 i
    int32_t j;        // 格坐标 j
    int64_t a;        // 原始坐标 a
    uint64_t b;       // 原始坐标 b
    uint8_t residual; // 残余 log 值
};

/// 单个 special-q 的筛结果
struct SieveResult {
    SpecialQ special_q;                     // special-q
    std::vector<SieveCandidate> candidates; // 候选点
    size_t sieved_positions = 0;            // 筛过的位置数
    size_t smooth_count = 0;                // 光滑数数量
};

/// 回调类型定义
using RelationCallback = std::function<void(Relation&&)>;
using ProgressCallback = std::function<void(size_t, size_t, const char*)>;

/// LatticeSieve - 格筛法主类
class LatticeSieve {
public:
    /// 构造函数
    /// @param ctx 多项式上下文
    /// @param fb 因子基
    /// @param params 筛法参数
    LatticeSieve(const PolynomialContext& ctx, const FactorBase& fb,
                 const SieveParams& params = SieveParams{})
        : ctx_(ctx), fb_(fb), params_(params), region_(default_sieve_region(ctx.skewness())) {}

    /// 显式执行配置构造；basis/adaptive policy 均不从 ENV 初始化.
    LatticeSieve(const PolynomialContext& ctx, const FactorBase& fb, const SieveParams& params,
                 const LatticeSieveExecutionConfig& config)
        : ctx_(ctx), fb_(fb), params_(params), region_(default_sieve_region(ctx.skewness())),
          execution_config_(config), adaptive_internal_(config.adaptive_lattice) {}

    /// 设置筛区域
    void set_region(const SieveRegion& region) {
        const int32_t width = region.i_width();
        const int32_t height = region.j_height();
        const size_t area = region.size();
        if (width <= 0 || height <= 0 || area == 0) {
            throw std::invalid_argument(
                "LatticeSieve region must have positive, representable dimensions");
        }

        // Allocate for the requested region before publishing it. A plain
        // resize() retains the previous capacity when the region shrinks; the
        // default region can be about 512 MiB while a 50-digit production
        // region needs only 16 MiB.
        std::vector<uint16_t> replacement(area, 0);
        region_ = region;
        sieve_array_.swap(replacement);
        last_init_val_ = 0; // 重置:不残留上次 SQ 的 estimate
    }

    /// Capacity currently reserved for the additive sieve array. This is a
    /// resource diagnostic, not the logical region size.
    [[nodiscard]] size_t allocated_sieve_bytes() const noexcept {
        return sieve_array_.capacity() * sizeof(uint16_t);
    }

    /// Logical cells in the configured sieve region. Unlike capacity, this
    /// value is exact across standard-library allocation strategies.
    [[nodiscard]] size_t sieve_cell_count() const noexcept {
        return sieve_array_.size();
    }

    /// 设置最大线程数。Legacy 的 0 = hardware auto；显式配置的 0 使用其
    /// deterministic fallback_thread_count。
    void set_max_threads(size_t n) {
        max_threads_ = n;
    }

    /// Return the configured cap. Zero delegates to the constructor policy.
    [[nodiscard]] size_t configured_max_threads() const noexcept {
        return max_threads_;
    }

    /// 测试钩子: 强制大素数也走 row_major (用于 bucket vs row-major 字节级一致性比对)
    /// 默认 false: 当 large_primes.size() >= 100 走 bucket region。
    /// 仅测试代码使用,production 路径请保持默认。
    void set_force_row_major(bool force) {
        force_row_major_ = force;
    }

    /// Test hook: fail after this many successful parallel thread launches.
    /// This models a std::thread construction failure.
    void set_sieve_parallel_launch_failure_after_for_testing(size_t successful_launches) noexcept {
        sieve_parallel_launch_failure_after_for_testing_ = successful_launches;
    }

    /// 设置关系回调
    void set_relation_callback(RelationCallback callback) {
        relation_callback_ = std::move(callback);
    }

    /// 设置进度回调
    void set_progress_callback(ProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

    /// Inject an external AdaptiveBasisManager (defaults to a per-instance
    /// internal one driven by ENV). Pass `nullptr` to fall back to the
    /// instance default. Used by `sieve_parallel` so all worker copies share
    /// telemetry through one manager.
    void set_adaptive_manager(AdaptiveBasisManager* mgr) noexcept {
        adaptive_external_ = mgr;
    }

    /// Read-only accessor to the manager that this sieve uses.
    [[nodiscard]] const AdaptiveBasisManager& adaptive_manager() const noexcept {
        return adaptive_external_ != nullptr ? *adaptive_external_ : adaptive_internal_;
    }

    /// Validate every lattice basis that this special-Q can use without
    /// allocating pass-local state or updating adaptive telemetry. The
    /// zero-hit trajectory reaches every possible retry because retry basis
    /// selection depends only on the current basis, retry ordinal, seed, and
    /// q; observed hit counts can only stop that trajectory early.
    void preflight_special_q(const SpecialQ& sq) const {
        require_affine_special_q_(sq);
        if (sq.r == 0) {
            return;
        }

        const AdaptiveBasisManager& mgr = adaptive_manager();
        LatticeBasis basis = initial_basis_for_special_q_(sq, mgr);
        require_region_projection_(basis);

        const auto& adaptive = mgr.config();
        if (!adaptive.enabled || adaptive.max_retries <= 0) {
            return;
        }
        const auto cells = static_cast<uint64_t>(region_.size());
        for (int retry = 0; retry < adaptive.max_retries; ++retry) {
            auto next = mgr.try_perturb_and_rereduce(basis, 0, cells, retry);
            if (!next.has_value()) {
                break;
            }
            basis = *next;
            require_region_projection_(basis);
        }
    }

    /// 对单个 special-q 进行筛法
    /// 使用 row-major bucket sieve：预计算所有 FB 素数的格参数，
    /// 然后逐行处理（每行在 L1 cache 中热驻留，所有素数贡献完成后才移到下一行）。
    /// 相比旧的 per-prime 遍历，L1 miss 从 O(FB_size × j_height) 降到 O(j_height)。
    ///
    /// Adaptive lattice (ENV `GNFS_ADAPTIVE_LATTICE=1`, default OFF):
    /// after the first pass, if hit density (cells where accumulated log
    /// exceeds threshold) is below the configured threshold and retry budget
    /// remains, the basis is perturbed via a unimodular skew transform and
    /// the region is re-sieved. Stats accumulate in adaptive_manager().
    [[nodiscard]] SieveResult sieve_special_q(const SpecialQ& sq) {
        // The lattice basis represents an affine root modulo q. Projective
        // roots use the UINT32_MAX sentinel, while any r >= q is a
        // non-canonical affine representative. Reject both before preparing
        // sieve storage or invoking basis reduction.
        require_affine_special_q_(sq);

        SieveResult result;
        result.special_q = sq;

        // r=0 退化:Gauss-reduced basis = ((0,1), (q,0)),to_ab(i,j)=(jq, i),
        // 即 b=i 可为负。collect_candidates 过滤 b<=0 后 sieve 一半工作无效;
        // 同时 estimate_initial_log 用 typical_a≈0 会塌缩 log 估计。
        // r=0 仅在 q | f₀ 时出现,极罕见,直接 skip 损失可忽略。
        if (sq.r == 0) {
            return result; // empty candidates, special_q recorded
        }

        ensure_sieve_array_storage_();

        AdaptiveBasisManager& mgr =
            (adaptive_external_ != nullptr) ? *adaptive_external_ : adaptive_internal_;

        // 1. 计算初始格基. Explicit instances use their captured policy;
        // legacy instances retain per-special-q ambient ENV reads.
        LatticeBasis basis = initial_basis_for_special_q_(sq, mgr);

        // First pass: sieve with initial basis.
        sieve_region_once_(basis, sq, result);

        // Adaptive retry path: only entered when manager is enabled.
        // When disabled, the next call returns nullopt instantly (single bool
        // check) and the loop body is never executed.
        if (mgr.config().enabled) {
            mgr.mark_special_q_processed();
            const uint64_t cells = static_cast<uint64_t>(region_.size());
            uint64_t hits = static_cast<uint64_t>(result.candidates.size());
            mgr.record_hit_stats(basis, hits, cells);

            int retry = 0;
            bool any_retry_adopted = false;
            const double threshold = mgr.config().density_threshold;
            while (true) {
                auto opt = mgr.try_perturb_and_rereduce(basis, hits, cells, retry);
                if (!opt.has_value())
                    break;

                mgr.mark_retry_attempted();
                basis = *opt;
                SieveResult retry_result;
                retry_result.special_q = sq;
                sieve_region_once_(basis, sq, retry_result);

                uint64_t new_hits = static_cast<uint64_t>(retry_result.candidates.size());
                mgr.record_hit_stats(basis, new_hits, cells);

                // No-regression guarantee: only adopt retry if it improves hits.
                // "Rescue" = perturbation produced a strictly better basis.
                if (new_hits > hits) {
                    result = std::move(retry_result);
                    hits = new_hits;
                    any_retry_adopted = true;
                }

                if (static_cast<double>(hits) / static_cast<double>(cells) >= threshold) {
                    break;
                }
                ++retry;
            }
            // Telemetry accounting:
            //   rescue = retry result was adopted (hits strictly improved by
            //            a perturbed basis).
            //   skipped = density still below threshold even after retries.
            // These are non-exclusive — a rescue can still leave density
            // below threshold for hard SQs.
            if (any_retry_adopted) {
                mgr.mark_rescue_succeeded();
            }
            if (static_cast<double>(hits) / static_cast<double>(cells) < threshold) {
                mgr.mark_low_density_skipped();
            }
        }

        return result;
    }

private:
    static void require_affine_special_q_(const SpecialQ& sq) {
        if (!sq.is_affine()) {
            throw std::invalid_argument(
                "LatticeSieve requires an affine Special-Q with q > 1 and r < q");
        }
    }

    [[nodiscard]] LatticeBasis initial_basis_for_special_q_(const SpecialQ& sq,
                                                            const AdaptiveBasisManager& mgr) const {
        if (execution_config_.has_value()) {
            return mgr.get_initial(sq, ctx_.skewness(), execution_config_->lattice_basis);
        }
        return mgr.get_initial(sq, ctx_.skewness());
    }

    void require_region_projection_(const LatticeBasis& basis) const {
        if (!lattice_projection_fits_int64(basis, region_)) {
            throw std::overflow_error(
                "LatticeSieve region projects outside int64 candidate coordinates");
        }
    }

    /// Internal helper: run a single sieve pass over `region_` with `basis`,
    /// populate `result.candidates` and `result.sieved_positions`.
    /// All five sieve sub-phases (init array, build primes, global hits,
    /// row-major, bucket region, collect_candidates) live here.
    /// Caller is responsible for adaptive bookkeeping and retry decisions.
    void sieve_region_once_(const LatticeBasis& basis, const SpecialQ& sq, SieveResult& result) {
        require_region_projection_(basis);

        // 2. 初始化筛数组（memset(0)，加法筛）
        init_sieve_array(basis);

        // 3. 预计算所有 FB 素数的格参数
        auto primes = build_prime_entries(basis, sq);

        // CompactSmallPrime stores residues below the row width in int16_t,
        // so 32768 is its exact inclusive width boundary. Wider regions must
        // send the complete prime set through the region-bucket path before
        // any global/small/large split; that path uses uint16_t offsets only
        // within fixed 64K regions and therefore supports every int32 width.
        // Keeping this as an early return also prevents flags==2 entries from
        // being applied once here and a second time inside the bucket path.
        constexpr uint32_t max_compact_row_width = static_cast<uint32_t>(INT16_MAX) + 1U;
        const uint32_t sieve_width = static_cast<uint32_t>(region_.i_width());
        if (sieve_width > max_compact_row_width) {
            sieve_bucket_region(primes);
            result.candidates = collect_candidates(basis);
            result.sieved_positions = region_.size();
            return;
        }

        // 4. 两级筛法:
        //    小素数 (p < sieve_width): row-major 直接筛（每行命中多次，cache 友好）
        //    大素数 (p >= sieve_width): bucket region 筛（每行最多命中一次）
        //    这比全 bucket 快，因为小素数的 bucket scatter 开销大于直接写入。
        uint32_t split_bound = sieve_width;

        // 分离小/大素数
        // Reserve: split typically ~70/30 small/large for medium FBs (50d/60d).
        // Worst case both vectors sum to primes.size() → reserve full size on both.
        std::vector<PrimeEntry> small_primes, large_primes;
        small_primes.reserve(primes.size() * 3 / 4);
        large_primes.reserve(primes.size() / 4);
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                // Global hits: apply directly
                uint16_t lp = pe.log_p;
                for (size_t idx = 0; idx < sieve_array_.size(); ++idx)
                    sieve_array_[idx] += lp;
            } else if (pe.flags == 1 || pe.p < split_bound) {
                small_primes.push_back(pe);
            } else {
                large_primes.push_back(pe);
            }
        }

        // Phase A: row-major for small primes (excellent L1 cache locality)
        if (!small_primes.empty()) {
            sieve_row_major(small_primes);
        }

        // Phase B: bucket region for large primes (at most 1 hit per row)
        // force_row_major_ 是测试钩子,production 时永远 false。
        if (large_primes.size() >= 100 && !force_row_major_) {
            sieve_bucket_region(large_primes);
        } else if (!large_primes.empty()) {
            sieve_row_major(large_primes);
        }

        // 5. 收集候选点
        result.candidates = collect_candidates(basis);
        result.sieved_positions = region_.size();
    }

public:
    /// 并行处理多个 special-q
    /// @param special_qs 要处理的 special-q 列表
    /// @param num_threads 线程数 (0 = auto)
    /// @return 所有 special-q 的合并结果
    [[nodiscard]] std::vector<SieveResult> sieve_parallel(const std::vector<SpecialQ>& special_qs,
                                                          size_t num_threads = 0) {

        if (special_qs.empty()) {
            return {};
        }

        if (num_threads == 0) {
            num_threads = execution_config_.has_value() ? resolve_internal_thread_count_()
                                                        : legacy_hardware_thread_count_();
        }
        num_threads = std::min(num_threads, special_qs.size());

        std::vector<SieveResult> all_results(special_qs.size());
        std::atomic<size_t> next_sq{0};

        // BACKLOG #4: optional E-core threads via GNFS_SIEVE_ECORE_THREADS=N.
        // SQ-level work-stealing — each SQ ~independent, faster cores grab more.
        const size_t ecore_count = resolve_ecore_thread_count_(num_threads);

        // Adaptive: share the host's manager across worker copies so telemetry
        // aggregates. When the manager is OFF (default), worker copies see
        // disabled state and skip all overhead.
        AdaptiveBasisManager& shared_mgr =
            (adaptive_external_ != nullptr) ? *adaptive_external_ : adaptive_internal_;

        auto process_special_qs = [&](LatticeSieve& local_sieve) {
            local_sieve.set_region(region_);
            local_sieve.set_adaptive_manager(&shared_mgr);

            while (true) {
                size_t idx = next_sq.fetch_add(1, std::memory_order_relaxed);
                if (idx >= special_qs.size())
                    break;

                all_results[idx] = local_sieve.sieve_special_q(special_qs[idx]);
            }
        };

        // Worker function - each thread gets its own LatticeSieve copy.
        // Construct each branch directly: AdaptiveBasisManager contains
        // non-movable atomics, so a conditional temporary is not viable.
        // No mutex needed: each thread writes to a unique all_results[idx].
        auto worker = [&](size_t worker_index) {
            gnfs::util::set_current_thread_qos(
                qos_for_sieve_thread(worker_index, num_threads, ecore_count));
            if (execution_config_.has_value()) {
                LatticeSieve local_sieve(ctx_, fb_, params_, *execution_config_);
                process_special_qs(local_sieve);
            } else {
                LatticeSieve local_sieve(ctx_, fb_, params_);
                process_special_qs(local_sieve);
            }
        };

        if (sieve_parallel_launch_failure_after_for_testing_.has_value()) {
            size_t successful_launches = 0;
            auto thread_launcher = [&](size_t, auto&& task) -> gnfs::util::JoiningThread {
                if (successful_launches >= *sieve_parallel_launch_failure_after_for_testing_) {
                    throw std::system_error(
                        std::make_error_code(std::errc::resource_unavailable_try_again),
                        "LatticeSieve test hook: parallel thread launch failed");
                }
                ++successful_launches;
                return gnfs::util::JoiningThread(std::forward<decltype(task)>(task));
            };
            gnfs::util::joined_worker_group_detail::run_joined_worker_group_with_launcher(
                num_threads, worker, thread_launcher);
        } else {
            gnfs::util::run_joined_worker_group(num_threads, worker);
        }

        return all_results;
    }

private:
    const PolynomialContext& ctx_;
    const FactorBase& fb_;
    SieveParams params_;
    SieveRegion region_;

    std::vector<uint16_t> sieve_array_; // 加法筛：累积 log_p 值
    uint16_t last_init_val_ = 0;        // 当前 SQ 的初始 log 估计值
    size_t max_threads_ = 0;            // 0 delegates to the constructor policy
    bool force_row_major_ = false;      // 测试钩子: 强制大素数走 row_major
    std::optional<size_t> sieve_parallel_launch_failure_after_for_testing_;

    RelationCallback relation_callback_;
    ProgressCallback progress_callback_;

    // Present only for the explicit constructor. The legacy constructor leaves
    // this empty so execution continues to use the historical ENV/host policy.
    std::optional<LatticeSieveExecutionConfig> execution_config_;

    // Adaptive lattice manager. By default each instance gets its own internal
    // manager driven by ENV at construction. `sieve_parallel` injects a shared
    // external manager so all worker copies aggregate stats into one place.
    // When manager.config().enabled is false (default), zero-overhead path.
    AdaptiveBasisManager adaptive_internal_{};
    AdaptiveBasisManager* adaptive_external_ = nullptr;

    [[nodiscard]] static size_t legacy_hardware_thread_count_() noexcept {
        size_t count = std::thread::hardware_concurrency();
        return count == 0 ? 4 : count;
    }

    [[nodiscard]] size_t resolve_internal_thread_count_() const noexcept {
        if (max_threads_ > 0) {
            return max_threads_;
        }
        if (execution_config_.has_value()) {
            return std::max<size_t>(1, execution_config_->fallback_thread_count);
        }
        return legacy_hardware_thread_count_();
    }

    [[nodiscard]] size_t resolve_ecore_thread_count_(size_t num_threads) const noexcept {
        if (!execution_config_.has_value()) {
            return resolve_ecore_thread_count(num_threads);
        }
        if (num_threads <= 1) {
            return 0;
        }
        return std::min(execution_config_->ecore_thread_count, num_threads - 1);
    }

    [[nodiscard]] bool tiny_simd_enabled_() const noexcept {
        return execution_config_.has_value() ? execution_config_->enable_tiny_simd
                                             : detail::tiny_simd_enabled();
    }

    [[nodiscard]] bool bucket_prefetch_enabled_() const noexcept {
        return execution_config_.has_value() ? execution_config_->enable_bucket_prefetch
                                             : gnfs::sieve::bucket_prefetch_enabled();
    }

    void ensure_sieve_array_storage_() {
        const size_t required_size = region_.size();
        if (sieve_array_.size() == required_size) {
            return;
        }
        std::vector<uint16_t> replacement(required_size, 0);
        sieve_array_.swap(replacement);
    }

    /// 初始化筛数组（加法筛：零填充）
    /// 加法筛中数组从 0 开始，每个 FB 素数命中时 += log_p。
    /// 候选检测时比较 accumulated >= (init_val - threshold)。
    /// memset(0) 比 std::fill(init_val) 快：OS 级零页优化 + 无分支写入。
    void init_sieve_array(const LatticeBasis& basis) {
        last_init_val_ = estimate_initial_log(basis);
        std::memset(sieve_array_.data(), 0, sieve_array_.size() * sizeof(uint16_t));
    }

    /// 估计初始 log 值
    [[nodiscard]] uint16_t estimate_initial_log(const LatticeBasis& basis) const {
        // 估计 (a, b) 在区域中的典型大小
        // |a| ~ |i * e0 + j * e1|, |b| ~ |i * f0 + j * f1|
        // E[|i|] ≈ range/4 for symmetric distribution on [i_min, i_max]
        // E[j]   ≈ midpoint for [j_min, j_max] (j > 0)
        const int64_t i_span =
            static_cast<int64_t>(region_.i_max) - static_cast<int64_t>(region_.i_min);
        const int64_t j_midpoint_sum =
            static_cast<int64_t>(region_.j_max) + static_cast<int64_t>(region_.j_min);
        double typical_i = std::max(1.0, static_cast<double>(i_span) / 4.0);
        double typical_j = std::max(1.0, static_cast<double>(j_midpoint_sum) / 2.0);

        double typical_a = std::abs(typical_i * static_cast<double>(basis.e0) +
                                    typical_j * static_cast<double>(basis.e1));
        double typical_b = std::abs(typical_i * static_cast<double>(basis.f0) +
                                    typical_j * static_cast<double>(basis.f1));

        // 有理侧 (GNFS convention): |a - b*m|
        double m_val = ctx_.m().to_double();
        double rat_val = std::abs(typical_a - typical_b * m_val);

        // Guard: log2(0) = -Inf, static_cast<uint16_t>(-Inf) is UB
        if (rat_val < 1.0)
            rat_val = 1.0;
        double rat_log = std::log2(rat_val) * params_.log_scale;

        // 代数侧: |N(a,b)| ~ |a|^d * some_factor
        uint32_t d = ctx_.degree();
        double alg_val = std::pow(std::max(typical_a, 1.0), d); // clamp to avoid log2(0)
        if (alg_val < 1.0)
            alg_val = 1.0;
        double alg_log = std::log2(alg_val) * params_.log_scale;

        // 返回合并值（final guard against NaN/Inf from edge cases）
        double combined = rat_log + alg_log;
        if (!std::isfinite(combined) || combined < 0.0)
            return 0;
        return static_cast<uint16_t>(std::min(combined, static_cast<double>(UINT16_MAX)));
    }

    // ── Bucket sieve 数据结构 ──────────────────────────────────

    /// Bucket entry: 大素数在某行的命中记录（4 bytes，紧凑 cache-friendly）
    struct BucketEntry {
        uint16_t offset; // 行内偏移 (0..width-1)
        uint16_t log_p;  // log 贡献
    };

    /// 预计算的 FB 素数条目
    /// 存储格坐标映射参数，避免 per-row 重复计算 mod_inverse
    struct PrimeEntry {
        uint32_t p;     // 素数
        uint16_t log_p; // log 贡献
        uint16_t flags; // 0=normal, 1=u_zero (整行命中), 2=uv_zero (全局命中)
        uint64_t u_inv; // mod_inverse(u, p)，仅 flags==0 时有效
        int64_t v;      // 格参数 v (mod p)
        // Carry-forward 预计算字段
        int32_t delta;      // 行间增量 = (-v * u_inv) mod p
        int32_t i_mod_init; // j=j_min 时的 i_mod 初始值
        int32_t i_min_mod;  // i_min mod p (预计算避免 per-row 除法)
    };

    // ── 格参数计算 ──────────────────────────────────────────

    /// 计算有理侧 (u, v) 参数
    /// u = (e0 - f0·m) mod p, v = (e1 - f1·m) mod p
    /// 格点 (i,j) 满足 p | (a - b·m) iff i·u + j·v ≡ 0 (mod p)
    [[nodiscard]] std::pair<int64_t, int64_t> compute_rational_uv(const LatticeBasis& basis,
                                                                  uint32_t p) const {
        // mpz_fdiv_ui returns floor-div remainder ∈ [0, p-1] (zero alloc, unified path)
        uint64_t m_mod_p = static_cast<uint64_t>(mpz_fdiv_ui(ctx_.m().get_mpz(), p));

        int64_t p64 = static_cast<int64_t>(p);
        auto mod_reduce = [p64](int64_t val) -> int64_t {
            int64_t r = val % p64;
            return r < 0 ? r + p64 : r;
        };
        int64_t e0_mod = mod_reduce(basis.e0);
        int64_t f0_mod = mod_reduce(basis.f0);
        int64_t e1_mod = mod_reduce(basis.e1);
        int64_t f1_mod = mod_reduce(basis.f1);
        int64_t m64 = static_cast<int64_t>(m_mod_p);
        int64_t u = (e0_mod -
                     static_cast<int64_t>(gnfs::util::mul_mod_u64(static_cast<uint64_t>(f0_mod),
                                                                  static_cast<uint64_t>(m64),
                                                                  static_cast<uint64_t>(p64))) +
                     p64) %
                    p64;
        int64_t v = (e1_mod -
                     static_cast<int64_t>(gnfs::util::mul_mod_u64(static_cast<uint64_t>(f1_mod),
                                                                  static_cast<uint64_t>(m64),
                                                                  static_cast<uint64_t>(p64))) +
                     p64) %
                    p64;
        return {u, v};
    }

    /// 计算代数侧 (u, v) 参数
    /// u = (e0 - f0·r) mod p, v = (e1 - f1·r) mod p
    /// 格点 (i,j) 满足 p | (a - b·r) iff i·u + j·v ≡ 0 (mod p)
    [[nodiscard]] std::pair<int64_t, int64_t> compute_algebraic_uv(const LatticeBasis& basis,
                                                                   uint32_t p, uint32_t r) const {
        int64_t p64 = static_cast<int64_t>(p);
        auto mod_reduce = [p64](int64_t val) -> int64_t {
            int64_t rem = val % p64;
            return rem < 0 ? rem + p64 : rem;
        };
        int64_t e0_mod = mod_reduce(basis.e0);
        int64_t f0_mod = mod_reduce(basis.f0);
        int64_t e1_mod = mod_reduce(basis.e1);
        int64_t f1_mod = mod_reduce(basis.f1);
        int64_t r64 = static_cast<int64_t>(r);
        int64_t u = (e0_mod -
                     static_cast<int64_t>(gnfs::util::mul_mod_u64(static_cast<uint64_t>(f0_mod),
                                                                  static_cast<uint64_t>(r64),
                                                                  static_cast<uint64_t>(p64))) +
                     p64) %
                    p64;
        int64_t v = (e1_mod -
                     static_cast<int64_t>(gnfs::util::mul_mod_u64(static_cast<uint64_t>(f1_mod),
                                                                  static_cast<uint64_t>(r64),
                                                                  static_cast<uint64_t>(p64))) +
                     p64) %
                    p64;
        return {u, v};
    }

    // ── 素数预计算 ──────────────────────────────────────────

    /// 为所有 FB 素数预计算格参数（mod_inverse + carry-forward delta）
    /// 每个 SQ 调用一次，结果供 sieve_row_major() 使用
    [[nodiscard]] std::vector<PrimeEntry> build_prime_entries(const LatticeBasis& basis,
                                                              const SpecialQ& sq) const {

        const auto& algebraics = fb_.algebraic();
        const size_t sieve_count = fb_.sieve_algebraic_count();
        if (sieve_count > algebraics.size()) {
            throw std::invalid_argument(
                "LatticeSieve algebraic sieve count exceeds the factor base");
        }

        std::vector<PrimeEntry> entries;
        entries.reserve(fb_.rational_count() + fb_.sieve_algebraic_count());

        const int32_t j_min = region_.j_min;
        const int32_t i_min = region_.i_min;

        const auto require_supported_sieve_entry = [](uint32_t p, uint32_t log_p) {
            if (p < 2 || p > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                throw std::invalid_argument(
                    "LatticeSieve factor-base primes must be in [2, INT32_MAX]");
            }
            if (log_p > static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())) {
                throw std::invalid_argument(
                    "LatticeSieve factor-base log contributions must fit in uint16_t");
            }
        };

        auto make_entry = [j_min, i_min](int64_t u, int64_t v, uint32_t p,
                                         uint32_t log_p) -> PrimeEntry {
            PrimeEntry pe;
            pe.p = p;
            pe.log_p = static_cast<uint16_t>(log_p);
            pe.delta = 0;
            pe.i_mod_init = 0;
            pe.i_min_mod = 0;
            if (u == 0 && v == 0) {
                pe.flags = 2;
                pe.u_inv = 0;
                pe.v = 0;
            } else if (u == 0) {
                pe.flags = 1;
                pe.u_inv = 0;
                pe.v = v;
            } else {
                pe.flags = 0;
                pe.u_inv = mod_inverse(static_cast<uint64_t>(u), p);
                pe.v = v;

                // Carry-forward 预计算:
                // delta = (-v * u_inv) mod p — 每行 i_mod 的增量
                int64_t p64 = static_cast<int64_t>(p);
                int64_t neg_v = (-v % p64 + p64) % p64;
                int64_t d = (neg_v * static_cast<int64_t>(pe.u_inv)) % p64;
                pe.delta = static_cast<int32_t>(d);

                // j=j_min 时的 i_mod 初始值
                int64_t rhs = (-static_cast<int64_t>(j_min) * v) % p64;
                if (rhs < 0)
                    rhs += p64;
                int64_t im = (rhs * static_cast<int64_t>(pe.u_inv)) % p64;
                pe.i_mod_init = static_cast<int32_t>(im);

                // i_min mod p (预计算)
                int64_t imin_mod = static_cast<int64_t>(i_min) % p64;
                if (imin_mod < 0)
                    imin_mod += p64;
                pe.i_min_mod = static_cast<int32_t>(imin_mod);
            }
            return pe;
        };

        // 有理侧 FB
        for (const auto& rp : fb_.rational()) {
            require_supported_sieve_entry(rp.p, rp.log_p);
            auto [u, v] = compute_rational_uv(basis, rp.p);
            entries.push_back(make_entry(u, v, rp.p, rp.log_p));
        }

        // 代数侧 FB（仅筛选范围，跳过投影根和 SQ 本身）
        for (size_t ai = 0; ai < sieve_count; ++ai) {
            const auto& ap = algebraics[ai];
            if (ap.is_projective())
                continue;
            if (ap.p == sq.q && ap.r == sq.r)
                continue;
            require_supported_sieve_entry(ap.p, ap.log_p);
            auto [u, v] = compute_algebraic_uv(basis, ap.p, ap.r);
            entries.push_back(make_entry(u, v, ap.p, ap.log_p));
        }

        return entries;
    }

    // ── Bucket Region Sieve ───────────────────────────────────
    // CADO-NFS-style: divide sieve area into L1-friendly regions,
    // scatter all prime hits in one pass, apply per-region.

    /// Bucket region entry: 4 bytes (offset within region + log_p)
    /// With 64K regions, offset fits uint16_t. log_p fits uint16_t (SIEVE_LOG_SCALE).
    struct BucketRegionEntry {
        uint16_t offset; // Position within region (0..region_size-1)
        uint16_t log_p;  // Log contribution
    };

    /// Compact bucket entry: 3 bytes (offset:16 + log_p:8)
    /// Saves 25% memory vs BucketRegionEntry for 80+ digit where bucket arrays are huge.
    /// log_p stored as uint8_t (max 255 after >>8 from uint16_t — suitable for SIEVE_LOG_SCALE ≤
    /// 256).
    struct CompactBucketEntry {
        uint16_t offset; // Position within region (0..region_size-1)
        uint8_t log_p;   // Truncated log contribution (>> 8 if needed)
    };

    /// Log2 of bucket region size. 2^16 = 65536 positions × 2B = 128KB ≈ L1 cache.
    static constexpr uint32_t LOG_BUCKET_REGION = 16;
    static constexpr uint32_t BUCKET_REGION_SIZE = 1u << LOG_BUCKET_REGION;

    /// Minimum total FB size to use bucket region sieve (below this, row-major is fine)
    static constexpr size_t BUCKET_REGION_FB_THRESHOLD = 5000;

    /// Bucket region sieve: scatter-then-apply for all primes.
    /// Phase 1 (scatter): iterate all primes once, compute hit positions, scatter to region
    /// buckets. Phase 2 (apply): for each region, apply all accumulated hits. Complexity:
    /// O(total_hits) instead of O(height × small_primes).
    void sieve_bucket_region(const std::vector<PrimeEntry>& primes) {
        const size_t total_area = sieve_array_.size();
        const size_t w = static_cast<size_t>(region_.i_width());
        const int32_t height = region_.j_height();
        const size_t height_size = static_cast<size_t>(height);
        const int32_t j_min = region_.j_min;

        // Number of bucket regions
        const size_t num_regions =
            total_area / BUCKET_REGION_SIZE + (total_area % BUCKET_REGION_SIZE != 0 ? 1 : 0);

        // Phase 0: global hits (flags==2, extremely rare).
        // NEON 8-lane via detail::apply_log_p_range (P2-A baseline).
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                detail::apply_log_p_range(sieve_array_.data(), total_area, pe.log_p);
            }
        }

        // Phase 1: Single-pass scatter into per-region vectors
        // No count pass needed — vectors grow dynamically.
        // This eliminates the 2× iteration over all primes that the old CSR approach required.

        constexpr uint32_t TINY_THRESHOLD = 256;

        // Collect medium+large prime indices for scatter
        std::vector<size_t> medium_prime_indices;
        medium_prime_indices.reserve(primes.size());
        for (size_t pi = 0; pi < primes.size(); ++pi) {
            if (primes[pi].flags == 0 && primes[pi].p >= TINY_THRESHOLD)
                medium_prime_indices.push_back(pi);
        }

        size_t scatter_threads = resolve_internal_thread_count_();
        if (medium_prime_indices.size() < 100) {
            scatter_threads = 1;
        } else {
            scatter_threads = std::min(scatter_threads, medium_prime_indices.size());
        }

        // Thread-local per-region bucket vectors (used by both serial and parallel paths)
        struct ThreadBuckets {
            std::vector<std::vector<BucketRegionEntry>> per_region;
        };
        std::vector<ThreadBuckets> thread_buckets(scatter_threads);
        for (auto& tb : thread_buckets) {
            tb.per_region.resize(num_regions);
        }

        // Scatter: each thread handles a chunk of primes
        {
            const size_t base_primes = medium_prime_indices.size() / scatter_threads;
            const size_t extra_primes = medium_prime_indices.size() % scatter_threads;

            // BACKLOG #4: optional E-core threads via GNFS_SIEVE_ECORE_THREADS=N.
            // Balanced scatter gives every launched worker a non-empty range.
            // Mixed P+E still hurts under the slowest-core barrier unless ENV
            // opts in.
            const size_t scatter_ecore_count = resolve_ecore_thread_count_(scatter_threads);

            auto scatter_worker = [&](size_t t) {
                const size_t start = t * base_primes + std::min(t, extra_primes);
                const size_t prime_count = base_primes + (t < extra_primes ? size_t{1} : size_t{0});
                const size_t end_idx = start + prime_count;
                const auto qos = qos_for_sieve_thread(t, scatter_threads, scatter_ecore_count);
                if (scatter_threads > 1) {
                    gnfs::util::set_current_thread_qos(qos);
                }
                auto& local = thread_buckets[t].per_region;
                // Resolve the prefetch gate once outside the inner loops:
                // it never changes mid-scatter and we want the inner
                // bodies to branch on a stack-local boolean rather than
                // re-issuing an atomic load per iteration.
                const bool prefetch_on = bucket_prefetch_enabled_();
                const size_t prefetch_step = kBucketPrefetchDistance;
                for (size_t pi_idx = start; pi_idx < end_idx; ++pi_idx) {
                    const auto& pe = primes[medium_prime_indices[pi_idx]];
                    int32_t p32 = static_cast<int32_t>(pe.p);
                    int32_t i_mod = pe.i_mod_init;
                    int32_t imin_mod = pe.i_min_mod;
                    for (int32_t row_offset = 0; row_offset < height; ++row_offset) {
                        int32_t offset = i_mod - imin_mod;
                        if (offset < 0)
                            offset += p32;
                        size_t row_base = static_cast<size_t>(row_offset) * w;
                        const size_t stride = static_cast<size_t>(pe.p);
                        const size_t row_end = row_base + w;
                        for (size_t pos = row_base + static_cast<size_t>(offset); pos < row_end;
                             pos += stride) {
                            size_t region_idx = pos >> LOG_BUCKET_REGION;
                            // Speculatively touch the destination region
                            // vector's metadata for an iteration far
                            // enough ahead that the L1 fill should land
                            // before we issue the next push_back. The
                            // prefetch is a hint — the output is bit-for
                            // -bit identical with or without it.
                            if (prefetch_on) {
                                const size_t look_pos = pos + prefetch_step * stride;
                                if (look_pos < row_end) {
                                    const size_t look_region = look_pos >> LOG_BUCKET_REGION;
                                    if (look_region < num_regions) {
                                        prefetch_bucket_write(
                                            static_cast<const void*>(&local[look_region]));
                                    }
                                }
                            }
                            if (region_idx < num_regions) {
                                local[region_idx].push_back(
                                    {static_cast<uint16_t>(pos & (BUCKET_REGION_SIZE - 1)),
                                     pe.log_p});
                            }
                        }
                        int64_t next_i_mod = static_cast<int64_t>(i_mod) + pe.delta;
                        if (next_i_mod >= p32)
                            next_i_mod -= p32;
                        i_mod = static_cast<int32_t>(next_i_mod);
                    }
                }
            };

            if (scatter_threads <= 1) {
                scatter_worker(0); // Inline for single-thread case
            } else {
                gnfs::util::run_joined_worker_group(scatter_threads, scatter_worker);
            }
        }

        // Phase 2: apply bucket regions + tiny prime stride

        // Build tiny prime list (reuse CompactSmallPrime).
        // Reserve: tiny primes are p < 256 → bounded count ~54 (PrimePi(256)).
        std::vector<CompactSmallPrime> tiny_primes;
        tiny_primes.reserve(64);
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p < TINY_THRESHOLD) {
                tiny_primes.push_back({pe.p, pe.log_p, static_cast<int16_t>(pe.delta),
                                       static_cast<int16_t>(pe.i_min_mod),
                                       static_cast<int16_t>(pe.i_mod_init)});
            }
        }

        // v-primes (flags==1 subset of primes — typically minor fraction).
        std::vector<VPrimeEntry> v_primes;
        v_primes.reserve(primes.size() / 8);
        for (const auto& pe : primes) {
            if (pe.flags == 1)
                v_primes.push_back({pe.p, pe.log_p});
        }

        // Apply per-region: bucket entries + tiny prime stride + v-primes
        // Each region writes to a non-overlapping sieve_array segment → thread-safe.
        auto apply_region = [&](size_t r) {
            size_t region_start = r * BUCKET_REGION_SIZE;
            size_t region_end =
                region_start + std::min<size_t>(BUCKET_REGION_SIZE, total_area - region_start);

            // Apply scattered bucket entries for this region (from all threads).
            // Gather hot loop: speculative prefetch of the `sieve_array_[pos]`
            // target `kBucketPrefetchDistance` entries ahead overlaps the
            // pointer-chase latency with the current XOR-add. Prefetch is a
            // hint only — bit-for-bit identical output regardless of state.
            const bool prefetch_on = bucket_prefetch_enabled_();
            const bool tiny_simd_on = tiny_simd_enabled_();
            for (size_t t = 0; t < scatter_threads; ++t) {
                const auto& vec = thread_buckets[t].per_region[r];
                const size_t n_entries = vec.size();
                for (size_t ei = 0; ei < n_entries; ++ei) {
                    if (prefetch_on) {
                        const size_t look = ei + kBucketPrefetchDistance;
                        if (look < n_entries) {
                            const size_t look_pos = region_start + vec[look].offset;
                            if (look_pos < total_area) {
                                prefetch_bucket_read(
                                    static_cast<const void*>(&sieve_array_[look_pos]));
                            }
                        }
                    }
                    const auto& entry = vec[ei];
                    size_t pos = region_start + entry.offset;
                    if (pos < total_area) {
                        sieve_array_[pos] += entry.log_p;
                    }
                }
            }

            // Apply tiny primes via stride within this region. Keep the row
            // interval as [0, height) offsets: a valid region may end at
            // INT32_MAX, where coordinate-based inclusive loops would
            // overflow on their final increment.
            const size_t region_row_begin = region_start / w;
            const size_t region_row_end = std::min((region_end - 1) / w + 1, height_size);

            // Make working copies for this region's i_mod state
            auto tiny_copy = tiny_primes;
            for (auto& tp : tiny_copy) {
                // Advance i_mod to region_row_begin.
                int32_t p32 = static_cast<int32_t>(tp.p);
                int64_t advanced =
                    static_cast<int64_t>(tp.i_mod) +
                    static_cast<int64_t>(region_row_begin) * static_cast<int64_t>(tp.delta);
                int64_t mod = advanced % static_cast<int64_t>(p32);
                if (mod < 0)
                    mod += p32;
                tp.i_mod = static_cast<int16_t>(mod);
            }

            for (size_t row_offset = region_row_begin; row_offset < region_row_end; ++row_offset) {
                const int32_t j = static_cast<int32_t>(static_cast<int64_t>(j_min) +
                                                       static_cast<int64_t>(row_offset));
                size_t row_base = row_offset * w;
                size_t row_end = row_base + w;
                // Clamp to region bounds
                size_t eff_start = std::max(row_base, region_start);
                size_t eff_end = std::min(row_end, region_end);
                if (eff_start >= eff_end)
                    continue;

                // v-primes: whole row. NEON 8-lane via detail::apply_log_p_range (P2-A baseline).
                for (const auto& vp : v_primes) {
                    if ((j % static_cast<int32_t>(vp.p)) == 0) {
                        detail::apply_log_p_range(sieve_array_.data() + eff_start,
                                                  eff_end - eff_start, vp.log_p);
                    }
                }

                // Tiny primes: stride within clamped range.
                // Loop fusion: v-prime broadcast above + tiny stride here run
                // back-to-back per row → sieve_array_ row stays L1-hot across
                // both passes. Stride writes go through detail::apply_log_p_stride
                // (4x unrolled, env-gated by GNFS_SIEVE_NO_TINY_SIMD).
                for (auto& tp : tiny_copy) {
                    int32_t off =
                        static_cast<int32_t>(tp.i_mod) - static_cast<int32_t>(tp.i_min_mod);
                    if (off < 0)
                        off += static_cast<int32_t>(tp.p);

                    const size_t stride = static_cast<size_t>(tp.p);
                    size_t idx = row_base + static_cast<size_t>(off);
                    // Skip to eff_start if needed.
                    if (idx < eff_start) {
                        size_t skip = (eff_start - idx + stride - 1) / stride;
                        idx += skip * stride;
                    }
                    detail::apply_log_p_stride(sieve_array_.data(), idx, eff_end, stride, tp.log_p,
                                               tiny_simd_on);

                    int32_t new_mod =
                        static_cast<int32_t>(tp.i_mod) + static_cast<int32_t>(tp.delta);
                    int32_t p32 = static_cast<int32_t>(tp.p);
                    if (new_mod >= p32)
                        new_mod -= p32;
                    tp.i_mod = static_cast<int16_t>(new_mod);
                }
            }
        };

        // Dispatch regions in parallel (no lock needed — disjoint writes).
        size_t num_threads = resolve_internal_thread_count_();
        if (num_regions < 4)
            num_threads = 1; // Not enough regions to parallelize
        num_threads = std::min(num_threads, num_regions);

        if (num_threads <= 1) {
            for (size_t r = 0; r < num_regions; ++r)
                apply_region(r);
        } else {
            // BACKLOG #4: optional E-core threads via GNFS_SIEVE_ECORE_THREADS=N.
            // Work-stealing (atomic fetch_add over region index) makes mixed P+E
            // robust to slowest-core barrier — faster cores grab more regions.
            const size_t ecore_count = resolve_ecore_thread_count_(num_threads);

            std::atomic<size_t> next_region{0};

            gnfs::util::run_joined_worker_group(num_threads, [&](size_t t) {
                const auto qos = qos_for_sieve_thread(t, num_threads, ecore_count);
                gnfs::util::set_current_thread_qos(qos);
                while (true) {
                    size_t r = next_region.fetch_add(1, std::memory_order_relaxed);
                    if (r >= num_regions)
                        break;
                    apply_region(r);
                }
            });
        }
    }

    // ── Row-major Bucket Sieve (original) ─────────────────────

    /// 预填充大素数的 per-row buckets
    /// 对 flags==0 且 p >= width 的素数，计算其在每行的命中位置并写入 bucket。
    /// 复杂度: O(large_primes × total_rows)，但 total_rows 仅 ~1K，所以很快。
    [[nodiscard]] std::vector<std::vector<BucketEntry>>
    fill_buckets(const std::vector<PrimeEntry>& primes, uint32_t bucket_threshold) const {

        const int32_t total_rows = region_.j_height();
        const auto w = static_cast<int32_t>(region_.i_width());

        std::vector<std::vector<BucketEntry>> buckets(static_cast<size_t>(total_rows));

        // 预估每行平均条目数用于 reserve
        size_t large_count = 0;
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p >= bucket_threshold)
                ++large_count;
        }
        // 每个大素数平均每行命中 w/p ≈ 0.3-1.0 次
        size_t avg_per_row =
            large_count * 2 / static_cast<size_t>(std::max(total_rows, static_cast<int32_t>(1))) +
            8;
        for (auto& b : buckets)
            b.reserve(avg_per_row);

        for (const auto& pe : primes) {
            if (pe.flags != 0 || pe.p < bucket_threshold)
                continue;

            int32_t i_mod = pe.i_mod_init;
            int32_t p32 = static_cast<int32_t>(pe.p);
            uint16_t lp = pe.log_p;

            for (int32_t row = 0; row < total_rows; ++row) {
                int32_t off = i_mod - pe.i_min_mod;
                if (off < 0)
                    off += p32;

                // p >= width → at most 1 hit per row
                if (off < w) {
                    buckets[static_cast<size_t>(row)].push_back({static_cast<uint16_t>(off), lp});
                }

                // Advance carry-forward
                int64_t next_i_mod = static_cast<int64_t>(i_mod) + pe.delta;
                if (next_i_mod >= p32)
                    next_i_mod -= p32;
                i_mod = static_cast<int32_t>(next_i_mod);
            }
        }

        return buckets;
    }

    /// Bucket sieve 主入口：分离小/大素数，预填充 bucket，多线程处理
    ///
    /// 小素数 (p < width): stride loop（多次命中/行，L1 cache 热驻留）
    /// 大素数 (p ≥ width): bucket apply（预排序命中，无空循环）
    void sieve_row_major(const std::vector<PrimeEntry>& primes) {
        const size_t w = static_cast<size_t>(region_.i_width());
        assert(w <= UINT16_MAX && "sieve width exceeds BucketEntry::offset uint16_t range");
        const uint32_t bucket_threshold = static_cast<uint32_t>(w);

        // Phase 0: 全局命中素数（u=0, v=0 → 每个位置都被整除，极罕见）
        for (const auto& pe : primes) {
            if (pe.flags == 2) {
                uint16_t lp = pe.log_p;
                for (size_t idx = 0; idx < sieve_array_.size(); ++idx)
                    sieve_array_[idx] += lp;
            }
        }

        // Phase 1: 预填充大素数 buckets
        auto buckets = fill_buckets(primes, bucket_threshold);

        // Pre-separate primes once (was per-thread-per-chunk)
        auto pre = preseparate_primes(primes, bucket_threshold);

        // Phase 2: 多线程行处理
        const int32_t total_rows = region_.j_height();
        const int32_t i_min = region_.i_min;

        size_t num_threads = resolve_internal_thread_count_();
        if (total_rows < 500)
            num_threads = 1;
        num_threads = std::min(num_threads, static_cast<size_t>(total_rows));

        if (num_threads <= 1) {
            sieve_row_chunk(pre, primes, buckets, bucket_threshold, 0, total_rows, w, i_min);
            return;
        }

        // 分块并行
        int32_t rows_per_thread = total_rows / static_cast<int32_t>(num_threads);
        int32_t remainder = total_rows % static_cast<int32_t>(num_threads);

        // Keep chunk boundaries as half-open row offsets in [0, total_rows).
        // Coordinate endpoints may sit at INT32_MIN/MAX, so a coordinate
        // one-past value is not generally representable.
        std::vector<int32_t> chunk_starts(num_threads + 1);
        chunk_starts[0] = 0;
        for (size_t t = 0; t < num_threads; ++t) {
            int32_t chunk_rows = rows_per_thread + (static_cast<int32_t>(t) < remainder ? 1 : 0);
            chunk_starts[t + 1] = chunk_starts[t] + chunk_rows;
        }

        // v18 优化: 单次性预算所有 chunk 的起始 i_mod (避免 N 线程独立扫 primes)
        auto chunk_imods = precompute_chunk_imods(pre, primes, bucket_threshold, chunk_starts);

        // BACKLOG #4: optional E-core threads via GNFS_SIEVE_ECORE_THREADS=N.
        // Static row-chunk dispatch — mixed P+E hurts unless ENV opts in (slowest
        // chunk barrier). Convert method-pointer style to lambda to inject QoS.
        const size_t row_ecore_count = resolve_ecore_thread_count_(num_threads);

        gnfs::util::run_joined_worker_group(num_threads, [&](size_t t) {
            int32_t chunk_begin_t = chunk_starts[t];
            int32_t chunk_end_t = chunk_starts[t + 1];
            const std::vector<int16_t>* imod_ptr = &chunk_imods[t];
            const auto qos = qos_for_sieve_thread(t, num_threads, row_ecore_count);

            gnfs::util::set_current_thread_qos(qos);
            sieve_row_chunk(pre, primes, buckets, bucket_threshold, chunk_begin_t, chunk_end_t, w,
                            i_min, imod_ptr);
        });
    }

    /// 紧凑小素数条目（16 bytes vs PrimeEntry 36 bytes → 2.25× cache 效率）
    /// INVARIANT: bucket_threshold must be <= INT16_MAX + 1 because stored
    /// values are < p.
    struct CompactSmallPrime {
        uint32_t p;
        uint16_t log_p;
        int16_t delta;     // fits in int16: delta < p < bucket_threshold
        int16_t i_min_mod; // fits in int16: i_min_mod < p
        int16_t i_mod;     // current carry-forward state, mutable per-thread
        // 12 bytes, padded to 12 (no need for 16)
    };

    /// v-prime (u=0: 整行命中) 条目
    struct VPrimeEntry {
        uint32_t p;
        uint16_t log_p;
    };

    /// Pre-separated prime lists (static fields only, built once per SQ)
    struct PreSeparatedPrimes {
        std::vector<CompactSmallPrime> small; // flags==0 && p < bucket_threshold
        std::vector<VPrimeEntry> v;           // flags==1
    };

    /// Build pre-separated lists from full PrimeEntry vector (call once per SQ)
    [[nodiscard]] PreSeparatedPrimes preseparate_primes(const std::vector<PrimeEntry>& primes,
                                                        uint32_t bucket_threshold) const {
        PreSeparatedPrimes result;
        result.small.reserve(primes.size());
        result.v.reserve(primes.size() / 8); // flags==1 typically minor fraction
        for (const auto& pe : primes) {
            if (pe.flags == 1) {
                result.v.push_back({pe.p, pe.log_p});
            } else if (pe.flags == 0 && pe.p < bucket_threshold) {
                result.small.push_back({
                    pe.p, pe.log_p, static_cast<int16_t>(pe.delta),
                    static_cast<int16_t>(pe.i_min_mod),
                    0 // i_mod filled per-chunk
                });
            }
        }
        return result;
    }

    /// 单次性预计算所有 chunk 的 i_mod 起始值 (v18 — chunk init 不再在并行段内)
    ///
    /// 旧实现: 每个线程独立扫 `primes` (~10k 素数), 对每个 small prime 计算
    ///   `i_mod = (i_mod_init + row_offset * delta) % p`
    /// → 8 线程 × 10k 素数 = 80k 个 64-bit mod, 还伴随 cache 重复扫 primes 数组。
    ///
    /// 新实现: 在调度前**单次**扫 primes (一次 cache 加载), 内层为 num_threads
    /// 个 chunk 计算各自的 i_mod, 整体 mod 总数相同但:
    ///   1. primes 只 stream 一次, L2/L3 cache 重用率高
    ///   2. 线程进入 sieve_row_chunk 后立刻可干活, 减少 wake-up 抖动
    ///   3. 为后续 Barrett 化 mod 替换留下统一切入点
    ///
    /// 返回: chunk_imods[t][si] = thread t 的第 si 个 small_prime 的 i_mod 起始值
    [[nodiscard]] std::vector<std::vector<int16_t>>
    precompute_chunk_imods(const PreSeparatedPrimes& pre, const std::vector<PrimeEntry>& primes,
                           uint32_t bucket_threshold,
                           const std::vector<int32_t>& chunk_starts) const {
        size_t num_chunks = chunk_starts.size() - 1;
        size_t num_small = pre.small.size();
        std::vector<std::vector<int16_t>> chunk_imods(num_chunks);
        for (auto& v : chunk_imods)
            v.resize(num_small);

        size_t si = 0;
        for (const auto& pe : primes) {
            if (pe.flags == 0 && pe.p < bucket_threshold) {
                int32_t p32 = static_cast<int32_t>(pe.p);
                int64_t delta = static_cast<int64_t>(pe.delta);
                int64_t init = static_cast<int64_t>(pe.i_mod_init);
                for (size_t t = 0; t < num_chunks; ++t) {
                    int64_t advanced = init + static_cast<int64_t>(chunk_starts[t]) * delta;
                    int64_t mod = advanced % static_cast<int64_t>(p32);
                    if (mod < 0)
                        mod += p32;
                    chunk_imods[t][si] = static_cast<int16_t>(mod);
                }
                ++si;
            }
        }
        return chunk_imods;
    }

    /// 处理一段半开行偏移范围 [row_begin, row_end)
    /// 小素数走 stride loop，大素数从 bucket 直接 apply
    ///
    /// @param initial_imods 预算好的 small_primes 起始 i_mod (chunk_imods[t]),
    ///                      nullptr 则回退到内嵌 mod 计算(单线程路径)
    void sieve_row_chunk(const PreSeparatedPrimes& pre, const std::vector<PrimeEntry>& primes,
                         const std::vector<std::vector<BucketEntry>>& buckets,
                         uint32_t bucket_threshold, int32_t row_begin, int32_t row_end, size_t w,
                         [[maybe_unused]] int32_t i_min,
                         const std::vector<int16_t>* initial_imods = nullptr) {

        // Copy pre-separated small primes, compute per-chunk i_mod
        assert(bucket_threshold <= static_cast<uint32_t>(INT16_MAX) + 1U &&
               "bucket_threshold exceeds int16_t range for CompactSmallPrime");
        auto small_primes = pre.small; // shallow copy (only i_mod differs)
        const auto& v_primes = pre.v;

        if (initial_imods != nullptr) {
            // 多线程路径: 调用方已预算各 chunk 的起始 i_mod, 直接拷贝
            assert(initial_imods->size() == small_primes.size());
            for (size_t si = 0; si < small_primes.size(); ++si) {
                small_primes[si].i_mod = (*initial_imods)[si];
            }
        } else {
            // 单线程回退路径: 现场算该行偏移的 i_mod。
            size_t si = 0;
            for (const auto& pe : primes) {
                if (pe.flags == 0 && pe.p < bucket_threshold) {
                    int32_t p32 = static_cast<int32_t>(pe.p);
                    int64_t advanced =
                        static_cast<int64_t>(pe.i_mod_init) +
                        static_cast<int64_t>(row_begin) * static_cast<int64_t>(pe.delta);
                    int64_t mod = advanced % static_cast<int64_t>(p32);
                    if (mod < 0)
                        mod += p32;
                    small_primes[si].i_mod = static_cast<int16_t>(mod);
                    ++si;
                }
            }
        }

        // Resolve the prefetch gate once per chunk: bucket apply runs
        // many times per row but the gate is process-global, so the
        // atomic load is hoisted to the chunk entry.
        const bool prefetch_on = bucket_prefetch_enabled_();
        const bool tiny_simd_on = tiny_simd_enabled_();

        for (int32_t row_offset = row_begin; row_offset < row_end; ++row_offset) {
            const int32_t j = static_cast<int32_t>(static_cast<int64_t>(region_.j_min) +
                                                   static_cast<int64_t>(row_offset));
            size_t row_base = static_cast<size_t>(row_offset) * w;
            size_t row_linear_end = row_base + w;

            // ── v-primes: whole-row broadcast (rare, 0-2 typical) ──
            // Route through NEON 8-lane helper; mirrors bucket-region apply
            // path so both code paths use the same vectorized broadcast.
            for (const auto& vp : v_primes) {
                if ((j % static_cast<int32_t>(vp.p)) == 0) {
                    detail::apply_log_p_range(sieve_array_.data() + row_base,
                                              row_linear_end - row_base, vp.log_p);
                }
            }

            // ── Small primes: stride loop via 4x-unrolled helper ──
            // Loop fusion: v-prime broadcast + small-prime stride share the
            // freshly-touched row cache line; this avoids a second pass over
            // sieve_array_ for the row.
            for (auto& sp : small_primes) {
                int32_t offset =
                    static_cast<int32_t>(sp.i_mod) - static_cast<int32_t>(sp.i_min_mod);
                if (offset < 0)
                    offset += static_cast<int32_t>(sp.p);

                const size_t idx = row_base + static_cast<size_t>(offset);
                const size_t stride = static_cast<size_t>(sp.p);
                detail::apply_log_p_stride(sieve_array_.data(), idx, row_linear_end, stride,
                                           sp.log_p, tiny_simd_on);

                // Advance carry-forward
                int32_t new_mod = static_cast<int32_t>(sp.i_mod) + static_cast<int32_t>(sp.delta);
                int32_t p32 = static_cast<int32_t>(sp.p);
                if (new_mod >= p32)
                    new_mod -= p32;
                sp.i_mod = static_cast<int16_t>(new_mod);
            }

            // ── 大素数: bucket apply (无分支，直接索引写入) ──
            // Gather hot loop: speculatively prefetch the upcoming
            // `sieve_array_` write target so the L1 fill lands before the
            // accumulator load issues. Prefetch is a hint — output is
            // bit-for-bit identical regardless of state.
            const auto& bucket = buckets[static_cast<size_t>(row_offset)];
            const size_t n_entries = bucket.size();
            for (size_t ei = 0; ei < n_entries; ++ei) {
                if (prefetch_on) {
                    const size_t look = ei + kBucketPrefetchDistance;
                    if (look < n_entries) {
                        prefetch_bucket_read(static_cast<const void*>(
                            &sieve_array_[row_base + bucket[look].offset]));
                    }
                }
                const auto& entry = bucket[ei];
                sieve_array_[row_base + entry.offset] += entry.log_p;
            }
        }
    }

    /// 收集候选点（加法筛 + NEON 向量化扫描）
    /// 加法筛中：candidate iff accumulated >= (init_val - threshold)
    /// 等价于旧减法筛中：residual <= threshold
    [[nodiscard]] std::vector<SieveCandidate> collect_candidates(const LatticeBasis& basis) const {
        std::vector<SieveCandidate> candidates;
        // Heuristic: ~0.5-2% of sieve cells pass threshold typically.
        // sieve_array_.size() / 100 is conservative; over-reserve worst case fine.
        candidates.reserve(sieve_array_.size() / 100);

        uint16_t threshold = params_.combined_threshold();
        // effective_threshold = init_val - threshold (if init too low, no valid candidates)
        if (last_init_val_ <= threshold) {
            return {}; // Log estimate too small — all positions would pass, return empty
        }
        uint16_t eff_thresh = static_cast<uint16_t>(last_init_val_ - threshold);

        auto process_hit = [&](size_t idx) {
            auto [i, j] = region_.index_to_ij(idx);
            auto [a, b] = basis.to_ab(i, j);
            if (b <= 0)
                return;
            if (std::gcd(util::safe_abs(a), b) != 1)
                return;

            SieveCandidate cand;
            cand.i = i;
            cand.j = j;
            cand.a = a;
            cand.b = static_cast<uint64_t>(b);
            // residual = init_val - accumulated (backwards compat)
            uint16_t acc = sieve_array_[idx];
            cand.residual = static_cast<uint8_t>(
                std::min(static_cast<uint16_t>(acc <= last_init_val_ ? last_init_val_ - acc : 0),
                         uint16_t(255)));
            candidates.push_back(cand);
        };

        const size_t n = sieve_array_.size();

#ifdef __ARM_NEON
        // NEON: scan 8 × uint16 per iteration, quick-reject blocks with no hits
        const uint16x8_t thresh_vec = vdupq_n_u16(eff_thresh);
        const size_t vec_end = n & ~size_t(7);

        for (size_t idx = 0; idx < vec_end; idx += 8) {
            uint16x8_t vals = vld1q_u16(&sieve_array_[idx]);
            uint16x8_t cmp = vcgeq_u16(vals, thresh_vec);
            // Quick reject: all 128 bits zero → no candidates in this block
            uint64x2_t cmp64 = vreinterpretq_u64_u16(cmp);
            if ((vgetq_lane_u64(cmp64, 0) | vgetq_lane_u64(cmp64, 1)) == 0)
                continue;
            // At least one hit — check individual lanes
            for (size_t k = 0; k < 8; ++k) {
                if (sieve_array_[idx + k] >= eff_thresh)
                    process_hit(idx + k);
            }
        }
        // Scalar tail
        for (size_t idx = vec_end; idx < n; ++idx) {
            if (sieve_array_[idx] >= eff_thresh)
                process_hit(idx);
        }
#else
        // Scalar fallback
        for (size_t idx = 0; idx < n; ++idx) {
            if (sieve_array_[idx] >= eff_thresh)
                process_hit(idx);
        }
#endif

        return candidates;
    }

    /// 计算模逆 (a^{-1} mod m)
    [[nodiscard]] static uint64_t mod_inverse(uint64_t a, uint64_t m) {
        // 扩展欧几里得算法
        int64_t t = 0, newt = 1;
        int64_t r = static_cast<int64_t>(m), newr = static_cast<int64_t>(a);

        while (newr != 0) {
            int64_t quotient = r / newr;
            t -= quotient * newt;
            std::swap(t, newt);
            r -= quotient * newr;
            std::swap(r, newr);
        }

        if (t < 0)
            t += static_cast<int64_t>(m);
        return static_cast<uint64_t>(t);
    }
};

} // namespace gnfs::sieve
