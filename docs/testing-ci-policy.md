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
   - Runs `BoundedChildProcess` and `SiqsShadowProofRssCampaignJournalStore` with a 120-second CTest timeout.
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
- `test_fixed_slot_executor`
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
sieve. Run it only through `./scripts/test.sh probe-50d-structured-ooc`,
`compare-50d-bounded-routes`, `compare-50d-first-round`, or
`probe-50d-special-q-workers`. These modes supply a hard special-Q cap, a
bounded outer-worker configuration, a local sieve compute-lane budget, fresh
processes, and isolated artifact directories. The worker mode runs settings 1,
2, and 4 under one fixed compute-lane budget; the two route modes compare
legacy and structured production paths. Every mode uses the closed
`GNFS_EXPERIMENT_V2` schema validator and treats timing and RSS as measurements,
not CI assertions.

`StructuredOOC50dContract` is a `fast` CTest that runs
`./scripts/test.sh --no-build check-50d-contracts`. It never enters the real
50-digit pipeline. It exercises all CLI rejection/help cases, asks the probe
emitter for one deterministic `GNFS_EXPERIMENT_FIXTURE_V2` pass fixture, rejects
any production-evidence prefix in that fixture, then runs the closed schema and
its synthetic negative mutations against it. CMake passes the selected target's
exact `$<TARGET_FILE:...>` and Python executable into the runner, so non-default
and multi-config build trees cannot accidentally validate a stale root
`build/` binary. This keeps CLI/schema drift in routine CTest without weakening
the disabled stress boundary above.

`test_ooc_durable_handoff` is split into two `instant` CTest entries. The core
suite fixes the canonical generic-handoff V1 and authorized-cleanup-marker V2
encodings, sealing, round-trip, zero-row, phase-kind separation, optional
duplicate-pending snapshot, and 64-KiB opaque-payload boundary. The negative
suite mutates every durable binding class and rejects malformed lengths,
versions, marker kinds, identities, extents, digests, truncation, trailing
bytes, and V1/V2 reinterpretation. This target is a pure protocol test: it
performs no filesystem mutation and grants no adoption or cleanup authority.

`test_ooc_cleanup_transaction` is split into four CTest entries.
`OOCCleanupTransactionCore` is an `instant` ownership and state-machine
contract covering move-only receipt consumption, repairable pending
publication, exact finalized expectations, a production writer/reader fixture,
SHA-256 marker corruption, ordered namespace states, injected rename/unlink
and parent-sync failures, foreign replacement, symlink/hardlink rejection,
reserved-name isolation, raw V2 rejection by the unchanged V1 runtime, and real
self-exec lock contention.
`OOCCleanupTransactionCrash` is a `fast` self-exec matrix that terminates the
child process at every canonical durable intent, quarantine,
delete-authorization, unlink, and intent-consumption boundary, then completes
recovery in the parent and reuses the same base. Pending-marker interruption
is tested in-process because an unspent ownership receipt is deliberately
non-serializable. After that process exits, pending-only state does not grant
recovery or fresh-reuse authority; a future durable ownership-token design must
close that boundary. The core suite also verifies that fresh writer creation
rejects every live, pending, canonical, staged, and quarantine namespace leaf.
RelationSink lease coverage verifies that the removable directory and every
pair transaction share one persistent external lock, that a real
cross-process contender reports `Busy`, and that the same base is reusable
without replacing that lock after a completed lease removal. It also replaces
an owned empty lease with a different live directory and verifies that the old
identity-bound receipt rejects the ABA target. The no-argument binary runs all
suites, so its `scripts/test.sh` tier and timeout are `fast` and 120 seconds.
`OOCCleanupAuthorityUnion` is an `instant` pure-policy suite from the same
binary. It exhaustively reduces all 60,025 combinations of four cleanup-marker
leaf states and two generic-handoff leaf states, plus namespace-foreign
dominance over the complete matrix. Foreign evidence precedes malformed or
wrong-role markers, which precede role-correct V2 records,
platform-limited handoff observations, and mixed legacy authorities. Every
V2-family or platform-limited observation rejects all current entry groups;
only an unblocked state delegates to the existing V1/C1 runtime. This suite
also freezes the pending-only `LegacyPendingCandidate` state: it carries no
cleanup authority and is valid only in the two pending slots. Raw filesystem
integration is covered separately by the core and private-lease crash suites.
The platform-limited metadata adapter is exercised directly for missing,
policy-compatible regular, invalid-mode POSIX, directory, hard-link, and
symlink leaves; it never reports `Exact`.
Those tests snapshot the complete test namespace and prove that role-correct
V2 records, malformed markers, foreign handoffs, exact handoff/V1 conflicts,
and partial V2 magic prefixes leave no namespace mutation. Static entry
placement keeps the preflight before sync, rename, rewrite, reconciliation, or
unlink. The deferred-writer integration additionally snapshots an open pair
and V2 leaf around rejection, proving its public cleanup-handoff preflight runs
before `finalize()` changes pair bytes; marker publication repeats the check.
The current runtime adapter combines all six logical leaf facts before
reduction, but its four cleanup-marker reads are not yet behind the same
private-directory handle as C1's handoff reads.
`OOCCleanupPrivateLeaseCrash` is a `fast` self-exec suite from the same
binary. It terminates children at each durable private-lease marker, rename,
and teardown boundary. It also covers writer termination after the first and
second `O_EXCL`, header validation, cleanup-receipt capture, and activation
commit. Preactivation recovery is interrupted after whole-directory
quarantine, both pair-leaf removals, owner and directory removal, and both
external marker removals. The suite verifies that links and unknown children
remain preserved, while a crash after activation keeps the live pair. The
suite also covers deferred worker cleanup handoff, pending-intent rollback,
canonical-intent cleanup, canonical intent with an unknown sibling, creator-PID
enforcement, and the POSIX fork rule that closing the parent receipt does not
unlock a still-running child. On macOS it also self-executes publisher and
adopter owner-death cases for exact private-handoff adoption, including
zero-row and partial-publication prefixes. Every adoption interruption and
namespace-replacement case checks fail-closed status, descriptor release, and
preservation of the no-delete handoff. It also proves that fork-inherited
adoption receipts and reader owners fail their current-process checks, that a
fork inside adoption cannot mint a second valid capability, and that the
parent capability remains valid. The generic-handoff classifier itself is
zero-mutation and accepts no cleanup flag; inspect, ordinary resume, and
pair-reuse checks preserve an exact duplicate pending leaf. Only the explicit
legacy lease recovery/removal transition may converge that exact duplicate or
discard an exact still-preactive pending-only publication. Other platforms
verify that adoption returns unsupported before observing or changing the
filesystem. The broader
`RelationReductionEngine`
integration target is `fast`: it repeatedly
creates and removes durable OOC leases, so its Debug single-run cost includes
the required file and parent-directory barriers.

Windows Release also runs a native sharing-violation retry branch; non-Windows
execution cannot substitute for that platform evidence.

`test_distributed_sieve` is the `fast` integration contract for the POSIX
worker pool. Its checks remain active under `NDEBUG`. In addition to serial
relation-set equivalence and chunk splitting, it verifies parent-numbered
attempts, first-attempt failure, cleanup-intent pending failure, corrupted
completion-descriptor and relation-sequence-receipt rejection, retry
exhaustion, full private-directory and external lease-marker removal, and
preservation of legacy raw worker leaves.
A worker becomes successful only after a confirmed `waitpid()`, complete OOC
descriptor-bound read, and exact lease cleanup. The suite validates the fixed
completion report's actual special-Q and persisted-relation counts separately
from post-merge rows. The parent recomputes the complete relation-sequence
receipt after its descriptor-bound read. The test does not claim that a
completed worker can be adopted after a master crash; the current contract
invalidates and recomputes the whole distributed wave.

`test_distributed_sieve_resume` is currently an `instant` pure protocol and
compile-boundary contract. Besides the closed record/dependency tests, it
compiles the source-private cleanup-authorization passkey and receipt traits.
The passkey and receipt have no production or test mint route, so these checks
freeze only the inaccessible capability surface; they are not evidence of a
durable WaveStore or cleanup authority.

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
