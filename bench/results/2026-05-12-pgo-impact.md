# PGO Impact Report — test_factor_with_kleinjung

**Date:** 2026-05-12
**Host:** Apple M5 (4P+6E, 4.61 GHz P-core)
**Build flags (baseline):** `-O3 -mcpu=native -flto=thin`
**Build flags (PGO):** `-O3 -mcpu=native -flto=thin -fprofile-instr-use=merged.profdata`
**Target binary:** `test_factor_with_kleinjung` (~30-bit GNFS factorization, full pipeline)

## Wall time (3 runs each)

| Build | Run 1 | Run 2 | Run 3 | Median | Min |
|---|---:|---:|---:|---:|---:|
| Baseline (Release, no PGO) | 55.06s | 49.22s | 47.99s | **49.22s** | 47.99s |
| PGO (Release + profile-use) | 48.19s | 48.24s | 47.98s | **48.19s** | 47.98s |
| **Δ median** | | | | **-2.09%** (PGO faster) | |
| **Δ min**    | | | | | **-0.02%** (within noise) |

**Observations:**
- Baseline run 1 (55.06s) is a cold-cache outlier; subsequent runs match PGO closely
- Steady-state wall time is essentially unchanged (~48s)
- PGO does eliminate the cold-cache outlier in this 3-run sample, but more runs are needed to claim that with statistical significance

## PMU TMA breakdown (Instruments "CPU Counters" / Bottlenecks mode)

xctrace's CPU Counters template emits a 4-column aggregated counter array per sample. Column labels are best-effort empirical (Apple doesn't document the column order):

| Column | A (baseline) | B (PGO) | Δ% |
|---|---:|---:|---:|
| col0_Discarded | 68,062,457 | 59,523,233 | **-12.55%** |
| col1_Processing | 461,436,053 | 435,557,369 | **-5.61%** |
| col2_Delivery | 18,977,092 | 16,669,756 | **-12.16%** |
| col3_Cycles | 21,373,063 | 17,590,863 | **-17.70%** |
| **Total** | **569,848,665** | **529,341,221** | **-7.11%** |

**Sample counts:** baseline 57,004 samples / PGO 52,952 samples (-7.1% — consistent with the aggregated counter Δ).

### Distribution shift (each column as % of total)

| Column | A% | B% | Δ pp |
|---|---:|---:|---:|
| col0_Discarded   | 11.94% | 11.24% | **-0.70pp** |
| col1_Processing  | 80.98% | 82.28% | **+1.31pp** |
| col2_Delivery    |  3.33% |  3.15% | **-0.18pp** |
| col3_Cycles      |  3.75% |  3.32% | **-0.43pp** |

PGO reduces the relative weight of three columns and increases Processing's share. This is consistent with PGO trimming dead/cold paths (Discarded, Delivery) while the backend Processing share — already 80% of the work — stays dominant.

## Verdict

✅ **PGO accepted as a working baseline.**

- Wall time does not regress (within run-to-run noise; PGO median is ~2% faster, min times nearly identical)
- All 4 PMU counter columns decrease in absolute terms (-5.6% to -17.7%); total -7.11%
- Sample count drops -7.1% (machine took fewer 1ms ticks to finish)
- Infrastructure (build → train → merge → use → trace → diff) end-to-end works

The wall-time improvement (~2%) is modest — at the low end of the doctrine §5.7 expected envelope (-5% to -20%). Likely causes:

1. **Training overfit**: `test_factor_with_kleinjung` is both training input and evaluation target. The two non-overlap training samples (`test_lattice_sieve`, `test_linalg`) finished in <0.2s each (instant tier), contributing little to the profile. The merged.profdata is dominated by patterns specific to a single 30-bit factorization.
2. **Already heavily optimized**: project compiles with `-O3 -mcpu=native -flto=thin`; algorithmic 90% wins are already harvested per RESOLVED.md v6-v21.
3. **Workload mix**: `test_factor_with_kleinjung` is a heterogeneous pipeline (poly select → factor base → sieve → cofactor → relations → linalg → sqrt). No single hot path dominates the wall time, so PGO's branch-prediction and code-layout gains are diluted.

## Recommendations

1. **Broaden training samples**: add `test_25digit`, `test_lattice_sieve` with non-trivial inputs (currently it runs only the instant path), and a representative subset of `test_gnfs_progressive L1-L3`. Estimated incremental work: +30 min train time, but potentially +3-5% PGO wall-time gain on diverse workloads.
2. **Keep PGO opt-in**: do NOT make it the default; the +2% gain doesn't justify the ~10-15 min train cost for every dev build.
3. **Use PGO for release artifacts only**: stamp PGO-built binaries when publishing benchmark numbers or cutting a tagged release.
4. **Re-measure on larger N**: 30-bit factorization is small. On `test_25digit` (81-bit) the absolute time is minutes, where %-level improvements compound.

## Training samples used

- `test_factor_with_kleinjung` (full GNFS pipeline, ~30-bit) — 78.2s training run
- `test_lattice_sieve` (sieve hotpath, but only instant test path: 0.1s) — minimal contribution
- `test_linalg` (block Lanczos/Wiedemann unit tests, instant: 0.0s) — minimal contribution

Total training profile: `merged.profdata` 108K.

## Notes

- Trace files (`*.trace`) and XMLs are large (10MB+); kept in `bench/results/` (gitignored).
- This `.md` report is the only tracked artifact from this measurement.
- Baseline run 1 cold-cache outlier (55s vs 48s) influenced the median improvement; the min times agree within noise. Future measurements should warm up with a discard run.

## Reproduce

```bash
# Build baseline (Release, no PGO)
cd <repo-root>
cmake -B build-baseline-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-baseline-release -j$(sysctl -n hw.ncpu) --target test_factor_with_kleinjung

# Train PGO (5-15 min)
./scripts/test.sh pgo-train --clean

# Wall-time comparison
for i in 1 2 3; do /usr/bin/time -p ./build-baseline-release/test_factor_with_kleinjung; done
for i in 1 2 3; do /usr/bin/time -p ./build-pgo-use/test_factor_with_kleinjung; done

# PMU traces
./scripts/perf/profile-cpu.sh ./build-baseline-release/test_factor_with_kleinjung
./scripts/perf/profile-cpu.sh ./build-pgo-use/test_factor_with_kleinjung

# Diff report
python3 scripts/perf/parse-trace.py bench/results/<baseline>.xml bench/results/<pgo>.xml
```
