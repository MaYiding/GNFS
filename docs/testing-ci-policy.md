# Testing and CI Policy

This document is the source of truth for GNFS test timing tiers, CI selection, and timeout rules.

## Timing Baseline

The current baseline was measured on 2026-06-02 with AppleClang on macOS arm64, `GNFS_ENABLE_NATIVE_ARCH=OFF`, and separate Debug and Release CMake build directories.

| Build | Command | Result |
|---|---|---|
| Debug | `ctest --test-dir build-timing-debug --output-on-failure --parallel <cpu>` | 143 passed, 4 timeout-only failures, 2 disabled |
| Debug timeout rerun | `ctest --test-dir build-timing-debug --tests-regex "^(DistributedSieve\|Integration\|BucketPrefetch\|API)$" --parallel 1 --timeout 900` | 4 passed |
| Release | `ctest --test-dir build-timing-release --output-on-failure --parallel <cpu>` | 147 passed, 2 disabled |

The Debug failures were not assertion failures. They were timeout mismatches caused by high parallel CTest contention and hard per-test `TIMEOUT` properties. The affected tests passed when rerun individually:

| Test | Debug single-run | Release parallel | New tier |
|---|---:|---:|---|
| `DistributedSieve` | 16.98s | 5.07s | `fast` |
| `Integration` | 5.70s | 3.52s | `fast` |
| `BucketPrefetch` | 11.73s | 4.13s | `fast` |
| `API` | 71.01s | 11.90s | `slow` |

Longest measured enabled tests:

| Test | Debug | Release | Tier |
|---|---:|---:|---|
| `FactorWithKleinjung` | 282.40s | 94.93s | `slow` |
| `Kleinjung` | 238.53s | 60.85s | `slow` |
| `RegressionGate` | 92.45s | 23.01s | `gate` |
| `KleinjungLarge` | 75.55s | 18.99s | `heavy` |
| `GNFS_E2E` | 46.60s | 3.64s | `slow` |

## Tier Rules

| Tier | Definition | CI placement |
|---|---|---|
| `instant` | Isolated unit or helper correctness tests. Single-run target is under 5s on Debug. | Cross-platform PR matrix, ASan/UBSan, coverage; selected concurrency tests under TSan |
| `fast` | Medium integration or resource-sensitive helper tests. Single-run target is under 30s on Debug. | Release PR matrix on Linux and macOS |
| `gate` | Multi-size correctness gate, especially 17/27/40/81-bit pipeline coverage. | Linux Release deep gate |
| `slow` | Real GNFS or API pipeline tests. Debug may take 30s to 5min. | Linux Release deep gate, nightly, local pre-merge |
| `heavy` | Long algorithmic or size-sensitive tests. | Manual, nightly candidate, never required PR |
| `bench` | Informational benchmark tests. | Benchmark workflow only, non-blocking |
| `stress` | 50/60-digit stress tests. | Manual or dedicated long-run workflow only |

Do not classify a test from Release timing alone. Debug, sanitizer, coverage, Windows, and high CTest parallelism can change runtime by an order of magnitude.

## CI Policy

The PR CI intentionally has two layers:

1. Cross-platform quick matrix:
   - Linux Release runs `instant|fast`.
   - Linux Debug runs `instant`.
   - macOS Release runs `instant|fast`.
   - Windows Release runs `instant`.
   - Linux arm64 runs `instant` as an experimental public-runner signal.
2. Linux Release deep gate:
   - Runs `gate|slow`, excluding `heavy|stress|bench`.
   - Uses `--parallel 1` because these tests compete for the same integer-heavy hot paths and can falsely timeout under high parallelism.

ASan/UBSan and coverage run only `instant` tests. They already multiply test cost through instrumentation, and they should not duplicate the Release deep gate. TSan uses the narrower lane below because its purpose is to exercise explicit concurrency boundaries, not to repeat every isolated helper.

## ThreadSanitizer Lane

Run the supported structured-relation race detector with:

```bash
./scripts/test.sh tsan-relation
```

The runner uses a dedicated `build-tsan-relation` Debug directory, disables native-architecture tuning, enables `GNFS_ENABLE_TSAN`, and builds only these targets:

- `test_ordered_parallel_map`
- `test_structured_parallel_prepare`
- `test_structured_batch_commit`
- `test_structured_parallel_driver`
- `test_structured_parallel_failures`
- `test_structured_incidence_builder`

The binaries run serially with a default 120-second timeout per binary. An explicit `--timeout` overrides that default. The Linux CI job has a separate 20-minute outer timeout, so configuration or compilation cannot leave the lane unbounded.

The runner supports Linux and macOS. On any other host it prints an explicit unsupported message, records the lane as skipped, and exits successfully. On Linux or macOS, CMake requires a Clang or GNU toolchain that can compile and link the ThreadSanitizer runtime; configuration fails instead of silently executing uninstrumented binaries when that contract is not met. `--no-build` likewise refuses a cache unless it records `GNFS_ENABLE_TSAN=ON`.

## Update Checklist

When adding or changing tests:

- Add the CTest entry with `LABELS` and `TIMEOUT`.
- Add the binary to `scripts/test.sh` `ALL_TEST_BINARIES`.
- Add matching entries to `TEST_TIMEOUT` and `TEST_TIER`.
- Put the test in `SMOKE_TESTS` only if it is pure `instant` and does not run a real GNFS pipeline.
- Put real pipeline tests in `MODULE_SLOW_TESTS` or a dedicated mode.
- For structured-relation concurrency changes, run `./scripts/test.sh tsan-relation` on a supported toolchain.
- Re-run at least `./scripts/test.sh list`, `ctest --show-only=json-v1`, and the affected local test subset.
