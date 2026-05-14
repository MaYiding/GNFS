# P1.B-1 Report — SpMV `__builtin_prefetch` Audit

**Date:** 2026-05-14
**Host:** Apple M5 (4P+6E, 4.61 GHz P-core)
**Tool stack:** mperf (PMU) + Apple `sample` (hot-symbol attribution)
**Branch:** `feat/260514-spmv-prefetch-audit`
**Spec:** doctrine §6 P1.B-1 (locked from 2026-05-13 PMU report)

## TL;DR

**Result: null on `test_factor_with_kleinjung`** — PMU showed no measurable change in `L1DMissRate` (12.76% → 12.88%, within noise) or wall time (46.79s → 47.82s, +2.2% within PET noise).

**Root cause is NOT a prefetch failure** — it's a **test-target / hot-path mismatch**:

| Layer | Status |
|---|---|
| Prefetch implementation | ✅ Correct (smoke 26/26, module linalg full PASS, test_block_wiedemann 7/7 incl. 5400×200 BW path) |
| PMU measurement on `test_factor_with_kleinjung` | ✅ Stable baseline reproduces (74.79% → 73.76% BackendStallRate; equivalent within noise) |
| SpMV code path actually executes? | ❌ **NO** — `BlockLanczos::find_dependencies` dispatcher routes to `find_dependencies_sparse` (Gaussian on `PackedGF2Matrix`) for any matrix where `m × (m+n) / 8 ≤ 4 GiB`. `test_factor_with_kleinjung` runs ≤50-bit semiprimes, all matrices fit. **BW SpMV is never called.** |

The change is retained as a future-proof optimization for `m × (m+n) > 4 GiB` workloads (e.g. `test_25digit`). doctrine §6 P1.B priorities shift below.

## 1. What was implemented

Two functions in `src/linalg/block_wiedemann.cpp`:

### 1.1 `bw_spmv_forward` (`block_wiedemann.cpp:17-39` after change)

```cpp
constexpr ptrdiff_t SPMV_PREFETCH_AHEAD = 8;

pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
    uint64_t acc = 0;
    const uint32_t* p_end  = M.row_end(i);
    const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                 ? p_end - SPMV_PREFETCH_AHEAD
                                 : M.row_begin(i);
    const uint32_t* p = M.row_begin(i);
    for (; p < p_pref; ++p) {
        __builtin_prefetch(&x.data[*(p + SPMV_PREFETCH_AHEAD)], 0, 0);
        acc ^= x.data[*p];
    }
    for (; p < p_end; ++p)
        acc ^= x.data[*p];                  // tail without prefetch
    y.data[i] = acc;
});
```

### 1.2 `bw_spmv_transpose` (`block_wiedemann.cpp:64-83` after change)

Same split-loop pattern, target is `local[*p]` (per-thread accumulator). `rw=0` chosen as baseline.

Design choices:
- `SPMV_PREFETCH_AHEAD = 8` — M5 L1D line=64 B, load-to-use~4 cy → 4-8 lines lookahead is the canonical window
- `locality=0` — SpMV is single-pass streaming, no L2 retention value
- Split-loop avoids per-iteration `p+N < p_end` check; tail loop handles rows shorter than N_AHEAD

Commits: `eab6245` (forward), `5dbce80` (transpose).

## 2. PMU baseline & after (`test_factor_with_kleinjung`)

Both runs `mperf -- $binary`, sudo for kpc API. **Same M5, same thermal, runs 60 s apart.**

| Metric | Baseline (recheck) | Prefetch N=8 | Δ |
|---|---:|---:|---:|
| Wall | 46.79 s | 47.82 s | +2.2% (PET noise) |
| Cycles | 92.82 G | 92.97 G | +0.16% |
| Instructions | 123.11 G | 123.10 G | -0.01% |
| **IPC** | **1.326** | **1.324** | -0.002 |
| **BackendStallRate** | **73.76%** | **73.76%** | **+0.00pp** |
| FrontendStallRate | 2.23% | 2.18% | -0.05pp |
| **L1DMissRate** | **12.76%** | **12.88%** | **+0.12pp** |
| TLBMissRate | 60.88% | 61.16% | +0.28pp |
| BranchMispredRate | 0.55% | 0.55% | +0.00pp |
| SIMDDensity | 5.84% | 5.85% | +0.01pp |

**Reading**: no significant change in any metric. PET-sampling noise (per 2026-05-13 §8) is on the order of ±5pp on rate metrics for the BackendStall layer; the observed Δ are an order of magnitude below.

PMU JSONs:
- `bench/results/2026-05-14-151531-test_factor_with_kleinjung-baseline_recheck.pmu.json`
- `bench/results/2026-05-14-151631-test_factor_with_kleinjung-prefetch_N8.pmu.json`

## 3. Why null — attribution via `sample`

20-second hot-symbol sample on `build-baseline-release/test_factor_with_kleinjung`:

| Symbol | Samples | CPU role |
|---|---:|---|
| `BlockLanczos::find_dependencies_sparse` (ThreadPool worker lambda) | **78,352** | Gaussian elimination on `PackedGF2Matrix` |
| `__psynch_cvwait` | 40,373 | Thread sleep / condition wait |
| `SQUFOF::factor` | 4,054 | Cofactorization |
| `LatticeSieve::sieve_row_chunk` | 2,012 | Sieve scatter |
| `find_dependencies_sparse` (direct frame) | 1,643 | Same as worker lambda |
| `__udivmodti4` | 786 | 128-bit divmod in cofactor |
| `MurphyEvaluator::compute_e_score_log` | 393 | Poly select scoring |
| `MatrixBuilder::build_with_qc` | 271 | Matrix construction |
| Block Wiedemann symbols | **0** | Never called |

**The hottest function by 10×+ is `find_dependencies_sparse`**, which is the **Gaussian elimination on `PackedGF2Matrix`** path inside `src/linalg/block_lanczos.cpp:104-218`. The dispatcher in `BlockLanczos::find_dependencies` (line 223-268) sends any matrix with `m × (m+n) ≤ 4 GiB` to this path — which is *every* matrix produced by `test_factor_with_kleinjung`.

The two functions we modified (`bw_spmv_forward`, `bw_spmv_transpose`) appear nowhere in the sample list. **0 samples = 0 execution time on this workload.**

## 4. Where the L1DMissRate 12.8% actually comes from

Since SpMV is not running, the 12.8% L1D miss must be in:

1. **Gaussian `aug.xor_rows(elim_rows[i], pivot_row)`** — random row access across the packed bit matrix. Each row is `(m+n)/64` 64-bit words contiguous, but row-to-row jumps in `elim_rows` are random. **Likely partial source**.
2. **Lattice sieve scatter** (`LatticeSieve::sieve_row_chunk`, 2k samples) — bucket scatter touches many factor base entries.
3. **Cofactor SQUFOF / classify** (~4k samples) — small but per-relation cache thrash.
4. **MurphyEvaluator alpha 78k-prime scan** — per BACKLOG.md known O(78k) hot loop.

The Gaussian aug-matrix size for `test_factor_with_kleinjung` (≤50-bit N) is bounded by O(few thousand rows × few thousand cols) → aug ≈ a few MB, **fits in L2 (8 MB on M5 P-core)**. So Gaussian itself is more L2-bound than L1-bound. The L1 miss source is more likely sieve scatter and cofactor cache thrash. Confirming this requires per-phase PMU breakdown (separate runs of `test_lattice_sieve`, `test_sqrt`, etc.) — deferred.

## 5. Why the BW SpMV change is retained (not reverted)

1. **Zero correctness risk** — split-loop is provably equivalent (tail handles short rows; main loop body is identical to baseline with one extra hint instruction).
2. **Zero overhead on the path that does run** — BW SpMV is dead code on this workload; the change cannot make things worse.
3. **Positive on the path that will eventually run** — for matrices with `m × (m+n) > 4 GiB` (e.g. `test_25digit`, future production 60-digit workloads), SpMV becomes the hot path and prefetch directly applies. Doctrine §6 P1.B-1 was originally written *in anticipation of* this path; the implementation is now in place for when validation becomes possible.
4. **Documented limitation** — this report and doctrine §6 explicitly mark the change as "implemented, pending validation on >4 GiB workload".

## 6. doctrine §6 P1.B priority shift (proposed)

Based on the sample attribution:

| Slot | Old priority | New priority | Rationale |
|---|---|---|---|
| P1.B-1 | SpMV prefetch | **Done, pending validation on test_25digit** | Implemented; never executes on small workloads |
| **P1.B-1b (new)** | — | **Gaussian xor_rows random-row prefetch + ThreadPool contention** | 78k samples (60%+ of CPU) + 40k mutex-wait samples |
| P1.B-2 | lattice_sieve align | Same priority | 2k samples — non-trivial but minor relative to Gaussian |
| P1.B-3 | TLB investigation | Same — deferred | Calibration still needed |

`__psynch_cvwait` 40k samples deserves its own BACKLOG entry: ThreadPool over-subscription / unnecessary blocking on small matrices is a *separate* MemBound-adjacent issue (idle threads stall the CPU's backend by holding state but doing no work, contributing to the 73.76% BackendStallRate).

## 7. Validation pathway forward

To prove the SpMV prefetch works (or doesn't), one of:

1. **`test_25digit` PMU run** — runs O(50k × 50k) matrices that exceed 4 GiB aug budget, forcing BW path. Heavy tier, ~1 h.
2. **Dedicated SpMV micro-benchmark** — load a saved CSR matrix (>10 GB), run 1000 SpMV iterations, PMU wall ~30 s. Requires new `bench_spmv.cpp`.
3. **Synthetic large matrix in `test_block_wiedemann`** — extend the existing 5400×200 case to e.g. 50000×30000 with adjustable iteration count. Cheapest path, but requires editing the test (not a benchmark file).

Option 2 is cleanest, deferred to a follow-up P1.B-1-validate task.

## 8. Decisions

- **Retain** `bw_spmv_forward` / `bw_spmv_transpose` prefetch changes (commits `eab6245`, `5dbce80`).
- **Do not** N_AHEAD scan — no signal to optimize against on this workload.
- **Update doctrine §6** to reflect Gaussian + mutex-wait as the real hot path; add P1.B-1b.
- **Add BACKLOG entry**: `[OPT] ThreadPool __psynch_cvwait dominance` (40k samples in 20s) — investigate over-subscription on small matrices.
- **Do not block merge** — change is correct, future-proof, and the analytic finding (target/hot-path mismatch) is itself valuable, doctrine-fulfilling work.

## 9. Reproduce

```bash
# Build
cmake -B build-spmv-prefetch-release -DCMAKE_BUILD_TYPE=Release
make -C build-spmv-prefetch-release -j$(sysctl -n hw.ncpu) test_factor_with_kleinjung

# PMU diff
sudo -E ./scripts/perf/pmu-stat.sh --out baseline_recheck build-baseline-release/test_factor_with_kleinjung
sudo -E ./scripts/perf/pmu-stat.sh --out prefetch_N8 build-spmv-prefetch-release/test_factor_with_kleinjung
python3 scripts/perf/pmu-derive.py \
    bench/results/*-baseline_recheck.pmu.json \
    bench/results/*-prefetch_N8.pmu.json

# Hot-symbol attribution
./build-baseline-release/test_factor_with_kleinjung > /tmp/run.log 2>&1 &
sample $! 20 -file /tmp/sample.txt > /dev/null
wait
less /tmp/sample.txt
```

## 10. Cross-reference

- `bench/results/2026-05-13-pmu-deepening.md` — P1.A baseline (this report's reference point)
- `docs/perf/performance-doctrine.md` §6 — to be updated with this finding
- `BACKLOG.md` — new entry `[OPT] ThreadPool __psynch_cvwait dominance` to be added
- `src/linalg/block_lanczos.cpp:223-268` — dispatcher routing logic
- `src/linalg/block_wiedemann.cpp:17-83` — modified SpMV functions
