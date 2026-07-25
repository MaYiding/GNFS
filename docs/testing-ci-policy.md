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

The PR CI intentionally has three layers:

1. Cross-platform quick matrix:
   - Linux Release runs `instant|fast`.
   - Linux Debug runs `instant`.
   - macOS Release runs `instant|fast`.
   - Windows Release runs `instant`.
   - Linux arm64 runs `instant` as an experimental public-runner signal.
   - Required glibc Linux rows set `GNFS_TEST_REQUIRE_AUTHENTICATED_LINUX=1`, so the sealed-image, same-object, descriptor-closure, and parent-death tests cannot silently skip a disabled authenticated transport.
2. Linux Release deep gate:
   - Runs `gate|slow`, excluding `heavy|stress|bench`.
   - Uses `--parallel 1` because these tests compete for the same integer-heavy hot paths and can falsely timeout under high parallelism.
3. Required Alpine Linux 3.21 musl transport-boundary lane:
   - Runs inside Docker on an Ubuntu host with the checkout mounted read-only at `/src`.
   - Installs only `build-base`, `cmake`, `gmp-dev`, and `linux-headers`.
   - Copies the checkout into a root-owned, mode-0700, container-native `/gnfs-src` test workspace because the journal-store test deliberately requires a trusted writable working directory.
   - Keeps the build directory and `TMPDIR` in the container-native `/tmp`; the host checkout remains read-only.
   - Configures `GNFS_BUILD_TESTS=ON`, `GNFS_BUILD_FUZZERS=OFF`, and `GNFS_ENABLE_NATIVE_ARCH=OFF`, then builds only the bounded-child and SIQS campaign-journal-store targets.
   - Runs `BoundedChildProcess` and `SiqsShadowProofRssCampaignJournalStore` with a 60-second CTest timeout.
   - Proves that the ordinary POSIX path transport builds and runs on musl, while production campaign admission rejects the unavailable authenticated profile without creating the journal lock, header, artifact entries, or launch marker.

ASan/UBSan and coverage run only `instant` tests. They already multiply test cost through instrumentation, and they should not duplicate the Release deep gate. TSan uses the narrower lane below because its purpose is to exercise explicit concurrency boundaries, not to repeat every isolated helper.

The authenticated Linux profile requires modern glibc plus its complete
syscall and macro surface. On unsupported libcs, store admission reports
`platform_unavailable` before opening the journal namespace, and direct
authentication reports the same error before opening or otherwise accessing
the executable path. Pure input-shape validation still precedes that platform
decision. A runtime capability rejection that is classified as unavailable on
an otherwise supported build retains its native cause in `native_error`. The
musl lane does not add an authenticated launch implementation and does not
change the authenticated transport ID.

## ThreadSanitizer Lane

Run the supported candidate, structured-relation, and SIQS shadow race detector
with:

```bash
./scripts/test.sh tsan-relation
```

The runner uses a dedicated `build-tsan-relation` Debug directory, disables native-architecture tuning, enables `GNFS_ENABLE_TSAN`, and builds only these targets:

- `test_ordered_parallel_map`
- `test_candidate_batch`
- `test_relation_collector`
- `test_relation_reduction_engine`
- `test_structured_parallel_prepare`
- `test_structured_batch_commit`
- `test_structured_parallel_driver`
- `test_structured_parallel_failures`
- `test_structured_incidence_builder`
- `test_siqs_shadow_linear_algebra`

The binaries run serially with a default 120-second timeout per binary. An explicit `--timeout` overrides that default. The Linux CI job has a separate 20-minute outer timeout, so configuration or compilation cannot leave the lane unbounded.

The runner supports Linux and macOS. On any other host it prints an explicit unsupported message, records the lane as skipped, and exits successfully. On Linux or macOS, CMake requires a Clang or GNU toolchain that can compile and link the ThreadSanitizer runtime; configuration fails instead of silently executing uninstrumented binaries when that contract is not met. `--no-build` likewise refuses a cache unless it records `GNFS_ENABLE_TSAN=ON`.

## Resource Measurement Lanes

`test_process_memory` is an `instant` cross-platform contract test. It checks
byte normalization and process high-water-mark monotonicity without requiring a
fixed allocation delta. `test_structured_ooc_50d_probe` is a disabled `stress`
target because it builds a real 50-digit factor base and runs the production
sieve. Run it only through `./scripts/test.sh probe-50d-structured-ooc`, which
supplies a hard special-Q cap, a bounded outer-worker configuration, a local
sieve compute-lane budget, and an isolated artifact directory. The companion
`probe-50d-special-q-workers` mode runs outer-worker settings 1, 2, and 4 in
fresh processes under one fixed compute-lane budget. Both modes treat timing
and RSS as measurements, not CI assertions.

`test_local_sieve_thread_budget` is the `instant` contract for balanced lane
allocation, invalid limits, and a bounded property grid. The 64-special-Q probe
remains a manual measurement and does not enter routine CI.

`test_candidate_batch_50d_sweep` is a disabled `bench;stress` CTest target. The
supported `./scripts/test.sh sweep-50d-candidate-batch [repetitions]` mode builds
it in Release, generates one real four-special-Q 50-digit candidate corpus, and
checks all 30 worker/chunk cases against a serial relation oracle. The runner
requires exactly 30 `GNFS_CANDIDATE_SWEEP_CASE_V1` records and one passing
`GNFS_CANDIDATE_SWEEP_SUMMARY_V1` record. Wall times are informational and do
not enter routine CI or a pass threshold.

`test_squfof_strategy_oracle` is a `fast` deterministic policy test. It builds
the fixed 192-case by 11-slot counterfactual matrix, verifies the frozen matrix
and stratified split identities, and solves the train-only exact subset DP.
Its iteration-count promotion gates are deterministic; it does not assert wall
time and does not change the production schedule.

`test_squfof_budget_corpus` is an `instant`, outcome-blind data contract. It
compiles the fixed-seed prospective budget corpus, independently verifies the
prime-factor metadata, and freezes a grouped train/validation/holdout split.
It deliberately neither includes nor calls SQUFOF, so the inputs and split are
committed before a budget policy observes their factor or iteration results.
The paired 3LP rows reproduce caller budgets but are not evidence for the
natural three-prime distribution; promotion claims must name their caller and
corpus scope explicitly.

`test_squfof_budget_oracle` is a `fast` deterministic offline policy gate. It
builds complete case-by-slot-by-cap matrices for the published V1 corpus and
the separately committed prospective corpus, then replays the production
budget, the preregistered absolute-cap candidate, and a train-only exact
fixed-order search. It asserts raw factor identity and integer work counts, not
wall time. Published V2 validation/holdout rows remain retrospective evidence;
only inputs sealed before the candidate's first probe are labeled prospective.
An offline result never changes the production path automatically.

`test_squfof_success_challenge_corpus` is an `instant`, outcome-blind data
contract for the next budget decision. It freezes 192 high-band normal-2LP
semiprimes across three factor-balance profiles, a grouped 2:1:1 split, and the
already selected absolute-cap candidate. It independently validates the known
prime factors and generator identity, but deliberately neither includes nor
calls SQUFOF. The corpus must be committed before either the production budget
or candidate cap is probed; its train split is not a new tuning set.

`test_squfof_success_challenge_oracle` is the paired `fast` two-cap policy
gate. It replays the production multiplier order for cap 20000 and the frozen
candidate 10056, checks every raw factor against independent `factor()` calls,
and aggregates integer work by sealed split and factor-balance profile. It does
not fit another policy or assert wall time. Any changed factor or new failure
is an immediate no-go; insufficient success coverage is reported only when
raw-factor correctness still holds. An offline result does not edit production.

`test_squfof_bench` is a disabled `bench` CTest target. The supported
`./scripts/test.sh bench-squfof [repetitions]` mode builds it in Release and runs
the fixed 50-digit SQUFOF strategy corpus. The runner validates every case,
multiplier, and summary record against one corpus, schedule, factor-result, and
failure-set identity. Wall times remain informational; the mode enforces no
performance threshold and does not enter routine CI.

`test_structured_ooc_scale --rss-case <rows> <workers>` is a manual measurement
mode, not a CTest performance assertion. Each invocation runs one scenario in a
fresh process and emits one `GNFS_RESOURCE_V1` record. The normal no-argument
gate remains a deterministic correctness test and has no timing or RSS
threshold.

`test_structured_filter_pipeline_120bit` is a `heavy` targeted size-transition
gate. It fixes a 120-bit semiprime and one polynomial/factor-base context, then
runs the production sieve-to-reduction route with a one-lane StandardV0 baseline
and a hardware-bounded structured route using at most four local-sieve and
incidence-building lanes. The test freezes the raw corpus, LP histogram, both
reduction outputs, and each full matrix's shape, NNZ, and canonical digest. It
disables the thin solver after matrix construction, and neither wall time nor
RSS is a pass criterion. The local `nightly` mode and scheduled nightly workflow
run it explicitly; it does not enter pull-request CI.

## Update Checklist

When adding or changing tests:

- Add the CTest entry with `LABELS` and `TIMEOUT`.
- Add the binary to `scripts/test.sh` `ALL_TEST_BINARIES`.
- Add matching entries to `TEST_TIMEOUT` and `TEST_TIER`.
- Put the test in `SMOKE_TESTS` only if it is pure `instant` and does not run a real GNFS pipeline.
- Put real pipeline tests in `MODULE_SLOW_TESTS` or a dedicated mode.
- For candidate, structured-relation, or SIQS shadow concurrency changes, run
  `./scripts/test.sh tsan-relation` on a supported toolchain.
- Re-run at least `./scripts/test.sh list`, `ctest --show-only=json-v1`, and the affected local test subset.
