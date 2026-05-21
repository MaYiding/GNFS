# GNFS Perf Sweep Automation

## Overview

The GNFS project ships roughly a dozen ENV-gated performance and algorithm
features (Bai-Brent polynomial selection, Brent-Suyama ECM, Block-Wiedemann
mmap and multi-stream, OOC relation store, Murphy alpha parallelism, V0_BFS
clique merge, lp_bits override, etc.). Each is conservative by default
because the ROI varies dramatically with input size. The sweep automation
gives a repeatable way to measure that ROI without hand-running every
combination.

Three scripts implement the workflow:

| Script | Purpose | Typical runtime |
|--------|---------|-----------------|
| `scripts/sweep_full.sh` | Single-variable sweep (each ENV vs baseline) | 40-bit ~ 5 min, 81-bit ~ 15 min, 50d hours |
| `scripts/sweep_combo.sh` | Pairwise 2×2 combo (A × B grid) | 4 runs per size |
| `scripts/sweep_analyze.py` | Cross-run aggregator across multiple sweep reports | Seconds |

All scripts respect the project's `--dry-run` and `--timeout` conventions
and write reports to `bench/results/`.

## Design Decisions

### Single-variable, not Cartesian

The 13 currently tracked ENVs would yield about 2^13 = 8192 combinations if
exhaustively swept, which is not tractable even at 5 seconds per run. The
default sweep instead varies one ENV at a time and compares to a clean
baseline. This surfaces each feature's standalone ROI clearly. For suspected
interactions (for example, `GNFS_BW_KRYLOV_MMAP` plus
`GNFS_BW_KRYLOV_STREAMS`) use the dedicated `sweep_combo.sh` script.

### Force GNFS path for bit-size runs

The `gnfs` CLI normally routes small N (under 25 digits) to SIQS by default.
That would bypass the sieve and linear algebra entirely, so the bit-size
runs in `sweep_full.sh` always inject `GNFS_DISABLE_SIQS=1`. This means the
sweep actually measures the GNFS code path, even on 40-bit inputs.

### Use existing N for fixed sizes

The bit-size N values are taken from `tests/test_regression_gate.cpp` so the
sweep matches existing regression coverage:

| Size | N | Source |
|------|---|--------|
| 27-bit | `100160063` (10007 × 10009) | L2 |
| 40-bit | `1000036000099` (1000003 × 1000033) | L3 |
| 50-bit | `100000980001501` (10000019 × 10000079) | test_factor_with_kleinjung |
| 81-bit | `1669994516749619561652133` (≈ 25-digit semiprime) | L4 |

For 50-digit and 60-digit sweeps the script delegates to
`test_stress 1 1` and `test_stress 2 2` respectively, which already
implement the proper trim limits and OOC scaffolding for those sizes.

## ENV Matrix

The default sweep matrix mirrors `CLAUDE.md`. To extend it, edit
`SWEEP_MATRIX` in `scripts/sweep_full.sh`.

| ENV | Default sweep values | Notes |
|-----|----------------------|-------|
| `GNFS_POLY_BAI_BRENT` | `1` | Non-monic Bai-Brent polynomial selection |
| `GNFS_ECM_BRENT_SUYAMA` | `1` | Brent-Suyama stage 2 ECM |
| `GNFS_ECM_BS_DEGREE` | `12,30` | Brent-Suyama polynomial degree |
| `GNFS_BW_KRYLOV_MMAP` | `1` | Block-Wiedemann Krylov mmap |
| `GNFS_BW_KRYLOV_STREAMS` | `2,4` | Parallel BW streams |
| `GNFS_MURPHY_ALPHA_THREADS` | `0,8` | Murphy alpha parallelism |
| `GNFS_OOC_RELATIONS` | `1` | OOC relation store |
| `GNFS_V0_BFS` | `1` | V0 BFS clique merge |
| `GNFS_V0_WEIGHT3` | `1` | V0 weight-3 merge |
| `GNFS_CASCADE_V3` | `auto,1` | V3 cascade merger |
| `GNFS_OVERRIDE_LP_BITS` | `20,22,24,26` | Override lp_bits default |
| `GNFS_SIEVE_ECORE_THREADS` | `0,4` | E-core QoS thread split |

The SIQS-routing ENVs (`GNFS_FORCE_SIQS`, `GNFS_DISABLE_SIQS`) are
excluded from the default bit-size matrix because `sweep_full.sh` passes
`--method gnfs` to the CLI, and an explicit `--method` overrides those
two ENVs in `Pipeline::select_method`. Use them only on digit-size
sweeps (`test_stress` does not pass `--method`) or temporarily inject
them with `--env-set` after editing the bit-size dispatcher to drop the
`--method` flag.

## Usage

### Daily quick sweep

```bash
./scripts/sweep_full.sh                  # default: 40 + 81 bit, all ENVs
./scripts/sweep_full.sh --bit 40         # one size only
./scripts/sweep_full.sh --env-set "GNFS_POLY_BAI_BRENT GNFS_BW_KRYLOV_STREAMS"
./scripts/sweep_full.sh --dry-run        # print plan, do not execute
./scripts/sweep_full.sh --no-build       # reuse existing build/
```

### Heavy sweep (50d, 60d)

These take hours per run. Launch in the background with `nohup` per the
project background-task convention, and bump the per-run timeout
accordingly.

```bash
nohup ./scripts/sweep_full.sh --digit 50 --timeout 7200 \
    > /tmp/sweep_50d.log 2>&1 &
echo "PID=$! LOG=/tmp/sweep_50d.log" >> /tmp/bg_tasks.txt
```

### Pairwise combo

```bash
./scripts/sweep_combo.sh --bit 81 \
    --env-a GNFS_POLY_BAI_BRENT \
    --env-b GNFS_ECM_BRENT_SUYAMA

./scripts/sweep_combo.sh --bit 40 \
    --env-a GNFS_BW_KRYLOV_STREAMS --val-a 4 \
    --env-b GNFS_BW_KRYLOV_MMAP --val-b 1
```

### Cross-run aggregation

```bash
python3 scripts/sweep_analyze.py bench/results/sweep_*.md
python3 scripts/sweep_analyze.py --json bench/results/*.md > aggregate.json
python3 scripts/sweep_analyze.py --top 5 --out aggregate.md \
    bench/results/sweep_*.md
```

## Report Format

`sweep_full.sh` emits a markdown file with three sections:

1. **Metadata** — host, git HEAD, build type, CPU count, ENV count.
2. **Per-N results** — one table per requested N size with rows
   `(ENV, value, wall time, status, Δ vs baseline)`.
3. **Best ENV per N size** — the single-variable winner at each size band.

Status values:

- `PASS` — exit code 0 within the timeout
- `FAIL` — exit code non-zero (does not contribute to "best" selection)
- `TIMEOUT` — watchdog killed the run (does not contribute)
- `DRY` — only emitted in `--dry-run` mode

`sweep_combo.sh` emits an interpretive table comparing
`A_only + B_only - baseline` (additive prediction) to the observed
`on-on` cell. Negative deviation suggests synergy, positive suggests
contention.

## Caveats

- Wall time on macOS includes background system activity. For tight
  measurements pin the CPU governor and close other apps. The sweep is
  most useful for spotting order-of-magnitude differences, not
  millisecond precision.
- Bit-size runs pass `--method gnfs --quiet` plus `GNFS_DISABLE_SIQS=1`
  so the pipeline always exercises the GNFS code path. `--method` takes
  precedence over `GNFS_FORCE_SIQS` and `GNFS_DISABLE_SIQS`, so any
  attempt to inject those two ENVs on a bit-size sweep is a no-op (they
  produce identical wall time to baseline). For digit-size sweeps
  (50d / 60d) the driver invokes `test_stress` directly, which does not
  pass `--method`, so SIQS-routing ENVs do take effect there.
- The 50d and 60d sweeps are hours long. Treat them as overnight
  experiments and always run with `nohup` plus `/tmp/bg_tasks.txt`
  tracking.
- The sweep does **not** validate factor correctness beyond exit code.
  Combine with `./scripts/test.sh gate` for that guarantee.
- The combo script tests only one value of each ENV at a time. For
  three-way or higher combinations either run multiple combo sweeps or
  extend the script.

## Extension Points

- New ENV: append `"GNFS_NEW_FLAG:val1,val2"` to `SWEEP_MATRIX` in
  `sweep_full.sh`.
- New N size: add an entry to `TEST_N` and `SIZE_TIMEOUT_DEFAULT`,
  then accept the new `--bit` or `--digit` value.
- Custom report layout: edit `render_markdown` in `sweep_full.sh` or
  re-pipe through `sweep_analyze.py`.

## Related Files

- `scripts/sweep_full.sh` — main sweep driver
- `scripts/sweep_combo.sh` — pairwise combo sweep
- `scripts/sweep_analyze.py` — cross-run aggregator
- `scripts/sweep_lp_bits.sh` — older lp_bits-specific sweep (precursor)
- `tests/test_sweep_smoke.sh` — shell-level smoke test for the sweep
  scripts
- `bench/results/sweep_*.md` — output reports
