# PMU Deepening Report — test_factor_with_kleinjung (M5)

**Date:** 2026-05-13
**Host:** Apple M5 (4P+6E, 4.61 GHz P-core)
**Tool:** [mperf](https://github.com/tmcgilchrist/mperf) (private kperf/kpep framework, MIT)
**Event database:** `/usr/share/kpep/as5.plist` (M5 native, ~80 exposed events)
**Builds:**
- baseline: `build-baseline-release/test_factor_with_kleinjung` — `-O3 -mcpu=native -flto=thin`
- PGO:     `build-pgo-use/test_factor_with_kleinjung`           — same + `-fprofile-instr-use=merged.profdata`

## 1. Why deeper events than the 2026-05-12 report

The 2026-05-12 PGO impact measurement used `xctrace`'s "CPU Counters" template, which on M-series aggregates into a **4-column** array (Discarded / Processing / Delivery / Cycles). Processing dominated at 80%+, but that single column **conflates MemBound and CoreBound** — and doctrine §6 P1 decision rules require separating those:

| doctrine §6 P1 rule | Required signal |
|---|---|
| MemBound (>30% backend + >5% L1D miss) | `ARM_STALL_BACKEND`, `L1D_CACHE_MISS_LD`, `ARM_MEM_ACCESS` |
| CoreBound (>30% backend + <2% L1D + <5% SIMD) | + `MAP_SIMD_UOP` |
| BadSpec (>5% mispred) | `BRANCH_MISPRED_NONSPEC`, `INST_BRANCH` |
| FrontendBound (>20% frontend) | `ARM_STALL_FRONTEND` |

Switching to mperf + `as5.plist` lets us read these directly. M5 has 2 fixed + 8 configurable counters → 10-event budget that exactly matches the slot count (no multiplexing).

## 2. Event set (final)

Order respects M5 `counters_mask` constraints (`INST_BRANCH` / `BRANCH_MISPRED_NONSPEC` have mask `0b11111100`, must go in slots 0/1; see commit `64b449f`):

```
Fixed:  FIXED_CYCLES, FIXED_INSTRUCTIONS
Config: INST_BRANCH, BRANCH_MISPRED_NONSPEC,        # mask-constrained, slots 0/1
        ARM_STALL_BACKEND, ARM_STALL_FRONTEND,
        L1D_CACHE_MISS_LD, L1D_TLB_MISS, ARM_MEM_ACCESS,
        MAP_SIMD_UOP
```

## 3. Raw counters

| Event | Baseline | PGO | Δ% |
|---|---:|---:|---:|
| `FIXED_CYCLES`            | 91,367,293,400  | 92,516,797,956  | +1.26% |
| `FIXED_INSTRUCTIONS`      | 123,148,640,290 | 122,695,781,823 | -0.37% |
| `INST_BRANCH`             | 25,677,403,243  | 25,707,390,835  | +0.12% |
| `BRANCH_MISPRED_NONSPEC`  | 141,167,736     | 139,931,398     | -0.88% |
| `ARM_STALL_BACKEND`       | 68,335,057,213  | 68,439,915,305  | +0.15% |
| `ARM_STALL_FRONTEND`      | 2,060,535,631   | 1,973,760,077   | -4.21% |
| `L1D_CACHE_MISS_LD`       | 2,764,714,331   | 2,732,829,617   | -1.15% |
| `L1D_TLB_MISS`            | 13,042,737,352  | 13,113,533,892  | +0.54% |
| `ARM_MEM_ACCESS`          | 21,593,778,225  | 21,277,890,175  | -1.46% |
| `MAP_SIMD_UOP`            | 7,207,267,170   | 7,332,571,844   | +1.74% |
| **Wall (ns)**             | 46,643,803,000  | 46,607,846,000  | -0.08% |
| **User (ns)**             | 225,046,064,000 | 222,087,898,000 | -1.31% |

## 4. Derived metrics (the actionable layer)

| Metric | Baseline | PGO | Δ |
|---|---:|---:|---:|
| **IPC**                | 1.348  | 1.326  | -0.022 |
| **BackendStallRate**   | **74.79%** | **73.98%** | -0.82pp |
| FrontendStallRate      | 2.26%  | 2.13%  | -0.12pp |
| **L1DMissRate**        | **12.80%** | **12.84%** | +0.04pp |
| TLBMissRate            | 60.40% | 61.63% | +1.23pp |
| BranchMispredRate      | 0.55%  | 0.54%  | -0.01pp |
| SIMDDensity            | 5.85%  | 5.98%  | +0.12pp |

## 5. doctrine §6 P1 decision

🎯 **MemBound triggered**: `BackendStallRate = 74.79%` ≫ 30% threshold AND `L1DMissRate = 12.80%` ≫ 5% threshold.

No other category fires:
- BadSpec fails: BranchMispredRate 0.55% (well under 5%)
- FrontendBound fails: FrontendStallRate 2.26% (well under 20%)
- CoreBound fails: L1DMissRate 12.80% > 2% disqualifies the "no L1 miss → pure compute" reading

**Three quarters of all backend-issue cycles are stalled, and ~13% of L1D loads miss.** The CPU spends most of its time waiting for memory — not waiting for execution units, not on branch mispred recovery, not on frontend fetch.

## 6. P1.B action plan (data-driven, locked from this report)

Per doctrine §6 P1 MemBound branch, prioritized by code hotness in GNFS pipeline:

### P1.B-1: `__builtin_prefetch` audit in Block Lanczos SpMV
**Why first**: Block Lanczos is the linalg main path (`src/linalg/block_lanczos.cpp`), and SpMV (`SparseGF2Matrix::multiply_block`) is the hottest inner loop in the linalg phase. SpMV in CSR layout is *the* canonical memory-bound kernel — irregular column-index access pattern blows L1D every iteration.
- Inspect `include/gnfs/linalg/packed_gf2_matrix.hpp` and `block_lanczos.cpp` for the column-index walking loop
- Insert `__builtin_prefetch(&v[col_indices[k + N_AHEAD]], 0, 0)` where N_AHEAD ≈ 8-16 (M5 L1D line = 64 B; load-to-use ≈ 4 cycles → ahead 4-8 lines)
- Expected: -15% to -30% on linalg wall time on matrices that exceed L1 working set

### P1.B-2: `lattice_sieve::scatter_bucket` cache-line align
**Why second**: Sieve scatter is the second-hottest memory-touching loop (`src/sieve/lattice_sieve.cpp`). Bucket sieve already partitions by region to reduce cache miss, but bucket entries themselves may straddle cache lines.
- Verify `Bucket` struct is 64 B aligned (`alignas(64)`)
- Check whether `region_size` is a multiple of 64 B
- Consider SoA layout for hot fields if AoS still shows misses

### P1.B-3: TLBMissRate investigation
**Why deferred but flagged**: TLBMissRate 60.40% is anomalously high. Either (a) `L1D_TLB_MISS` counts speculative+demand together inflating the numerator, or (b) the GNFS heap allocates across thousands of pages without huge-page hints. If real, this is a separate big lever.
- First reproduce on a pure-compute benchmark (small `test_integer`) to calibrate the metric
- If still >20% in a heap-heavy workload, consider `madvise(MADV_HUGEPAGE)` for the relation buffer and the Block Lanczos vectors

### Out of scope for P1.B (deferred):
- NEON full coverage — `SIMDDensity` is only 5.85%, but CoreBound is NOT triggered (memory is the bottleneck, not execution width). NEON work moves to P1.C only after MemBound is fixed and rerun shows backend stall < 30%.
- `[[likely]]/[[unlikely]]` annotations — `BranchMispredRate` 0.55%; no signal.

## 7. PGO impact, revisited with PMU eyes

The 2026-05-12 PGO report showed wall-time -2.09% (median, 3 runs) but couldn't explain *why*. Today's PMU data does:

| Layer | Baseline | PGO | Reading |
|---|---:|---:|---|
| Wall                  | 46.64s   | 46.61s    | -0.08% (noise) |
| Cycles                | 91.4 G   | 92.5 G    | +1.3% — PGO **slightly worse** |
| Instructions          | 123.1 G  | 122.7 G   | -0.4% — PGO trims a few branches |
| BackendStallRate      | 74.79%   | 73.98%    | -0.82pp — marginal |
| BranchMispredRate     | 0.55%    | 0.54%     | -0.01pp — already saturated |
| FrontendStallRate     | 2.26%    | 2.13%     | -0.12pp — minor improvement (best-case for PGO) |

**Verdict**: PGO's main levers are code layout and branch hints. On a workload that is **74% backend-stalled** (memory-bound), PGO has almost no surface area to attack. The -2% wall-time gain in the previous 3-run sample was within run-to-run noise (today's 1-run Δ is even smaller: -0.08%).

This is a **canonical PGO failure mode** documented in doctrine §3.6 ("compiler optimizations don't fix data-layout problems"). The 2026-05-12 decision to keep PGO opt-in stands; we're not seeing surprising new value.

## 8. PET-sampling caveat (interpretation guard rails)

mperf reported `threads_measured: 233` for baseline / `197` for PGO, and the run emitted "Too many threads, some data may be lost" warnings. This is a PET buffer limit, not a kernel error. Practical impact:

- **Absolute counts are under-counted by ~10×** — we measured 91.4 G cycles, but `wall × cores × freq ≈ user_ns × freq = 225 s × 4.61 GHz ≈ 1037 G cycles` would be the upper bound. mperf only captures a sampled slice when 233 threads compete.
- **Derived ratios are still trustworthy** — `ARM_STALL_BACKEND / FIXED_CYCLES`, `L1D_CACHE_MISS_LD / ARM_MEM_ACCESS`, etc. share the same PET sampling window, so the under-count factor cancels.
- **Decision validity**: the BackendStallRate (74.79%) and L1DMissRate (12.80%) signals are robust to PET sampling. They would have to flip by 50+ percentage points to invalidate the MemBound conclusion, which is unimaginable from buffer drops.

Future work to refine: drop thread count by serializing the test order, or use `mperf -p 5` to widen the sampling period (trades resolution for buffer fit).

## 9. Limitations & known caveats

1. **Single run, no median** — PGO 2026-05-12 used 3 runs/median; today is 1 run each. Within-noise differences (<3%) are not statistically meaningful here.
2. **Heterogeneous pipeline workload** — `test_factor_with_kleinjung` runs ~10 different test cases (8-bit to 50-bit semiprimes, base-m + Kleinjung), each with different hot paths. The aggregate metrics blend poly-select, sieve, linalg, sqrt. P1.B execution should re-profile **per-module** (e.g. `test_linalg` alone, `test_lattice_sieve` alone) to validate locally before patching.
3. **TLBMissRate sanity** — see §6 P1.B-3.
4. **mperf private-framework risk** — kperf API could change in a future macOS release.

## 10. Reproduce

```bash
# 1. One-time setup
./scripts/perf/install-mperf.sh

# 2. Ensure both release binaries exist
ls -lh build-baseline-release/test_factor_with_kleinjung build-pgo-use/test_factor_with_kleinjung

# 3. Capture (each ~50 s, sudo required for kpc API)
sudo -E ./scripts/perf/pmu-stat.sh --out baseline build-baseline-release/test_factor_with_kleinjung
sudo -E ./scripts/perf/pmu-stat.sh --out pgo      build-pgo-use/test_factor_with_kleinjung

# 4. Diff
python3 scripts/perf/pmu-derive.py \
    bench/results/*-test_factor_with_kleinjung-baseline.pmu.json \
    bench/results/*-test_factor_with_kleinjung-pgo.pmu.json
```

## 11. Cross-reference

- 2026-05-12 PGO impact report: `bench/results/2026-05-12-pgo-impact.md`
- doctrine §5 (Instruments + PGO loop): `docs/perf/performance-doctrine.md`
- P1.A plan: `docs/superpowers/plans/2026-05-13-pmu-events-deepening.md`
