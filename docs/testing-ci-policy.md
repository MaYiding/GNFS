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
| `slow` | Real GNFS or API pipeline tests. Debug may take 30s to 5min. | Linux Release deep gate, targeted platform-specific PR lanes, nightly, local pre-merge |
| `heavy` | Long algorithmic or size-sensitive tests. | Manual, nightly candidate, never required PR |
| `bench` | Informational benchmark tests. | Benchmark workflow only, non-blocking |
| `stress` | 50/60-digit stress tests. | Manual or dedicated long-run workflow only |

Do not classify a test from Release timing alone. Debug, sanitizer, coverage, Windows, and high CTest parallelism can change runtime by an order of magnitude.

### Release-active correctness checks

Release builds define `NDEBUG`, so the standard `assert()` macro is not a
correctness oracle in CI. New or modified correctness tests must use a check
that remains active in Release builds. The first migrated mathematical chain is
`Integer`, `IntPolynomial`, `LinearAlgebra`, and `SquareRoot`; these tests use
`GNFS_TEST_CHECK` from `tests/support/test_check.hpp` in every build type.
The `HalfGCD`, `PolyKaratsuba`, `DivremSubquadratic`, `PolyNTT`, and
`PolySquare` arithmetic contracts also keep their exact parity, dispatch,
boundary, and environment-parsing checks active in Release builds.
The `SafeMath` contract keeps its runtime checks for full-width signed
absolute values and non-finite conversion active under `NDEBUG`; its
saturation-boundary and other compile-time `static_assert` checks remain
unchanged.
The `KrylovSequenceMmap` persistence, handle-lifecycle, and size-boundary
contract uses the same Release-active check and exercises the real Win32
file-mapping path on Windows instead of a platform stub. Arithmetic or native
file-offset sizes that cannot be represented must fail before creating or
truncating the requested path.
The `BWKrylovMmapIntegration` contract requires every returned dependency to
be valid and preserves bit-for-bit results across memory, mmap, and mmap+zip
storage. Its trace assertions prove that neither on-disk route fell back to the
scalar solver and that compressed scratch data was reopened, copied, and
removed.
The `KrylovCompressor` byte-codec and cross-platform
`KrylovSequenceCompressed` contracts use Release-active checks for exact copy
round trips, hostile header and index rejection, deterministic cache eviction,
Unicode paths, exact-length publication, and terminal failure handling. The
required Windows row executes the real Win32 positioned-I/O backend instead of
a platform stub.
The `NativeRandomAccessFile` instant contract exercises exact positioned I/O,
range rejection, high-offset reads, Unicode paths, and move-only ownership. It
runs the Win32 overlapped path in the required Windows row and `pread`/`pwrite`
on POSIX; it is not a pathname-identity or durable-publication contract.

Do not remove `NDEBUG` globally. Fresh-process probes and performance campaigns
deliberately exercise the optimized Release contract. Migrate legacy unit tests
to release-active checks in bounded batches instead.

## CI Policy

The PR CI intentionally has three layers:

1. Cross-platform quick matrix:
   - Linux Release runs `instant|fast`.
   - Linux Debug runs `instant`.
   - macOS Release runs `instant|fast` and the targeted
     `test_distributed_sieve_worker_cleanup_tail` slow contract. That extra
     invocation is the CI witness for the Apple-only same-OFD lock, fork
     rejection, and cleanup-intent conversion path; unsupported hosts execute
     only the platform stub.
   - Windows Release runs `instant` plus the targeted fast
     `BWKrylovMmapIntegration` contract. This required end-to-end witness
     exercises the real Win32 raw-mapping and compressed positioned-I/O paths
     through Block Wiedemann.
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

`CLIEventStream` is an `instant` real-process contract. It launches the selected
`gnfs` target with successful, verbose, invalid, and incompatible invocations;
then it validates JSON Lines framing, event order, terminal status, and process
exit status. The cross-platform quick matrix runs it on Windows as well as POSIX
hosts, so `CreateProcess`, runtime DLL lookup, complete child-environment
replacement, and executable-path handling remain covered. The test is also in
the project smoke set because each invocation uses a deterministic small input.

## Nightly and Release Qualification

The scheduled nightly workflow and the verify-only release phase call the same
reusable qualification workflow. This keeps expensive coverage outside the
default pull request matrix while preventing the release path from drifting
away from nightly evidence. The reusable workflow runs three independent
Release jobs: the repository `thorough` mode, the structured 120-bit route
gate, and the bounded four-special-Q 50-digit legacy/structured route
comparison. The bounded comparison uses a 900-second timeout for each route;
it is not a complete 50-digit factorization claim. Successful probes retain
the permanent zero-byte OOC coordination locks required by the store contract.
The Harness removes the private test directory only after a no-following,
directory-relative check proves that its entries are exactly the raw lock and,
for the structured route, one precisely named structured-output lock. Extra,
renamed, linked, symlinked, or nonempty residue remains a release failure.

Release publication also requires every workflow that was triggered by the
exact main push to finish successfully. A fixed set of release-critical jobs
is verified against both the Actions workflow-run API and the commit check-run
API. The complete publication procedure and artifact contract are documented
in [releasing.md](releasing.md).

Workflows that supply those fixed release-critical jobs run on every `main`
push without `paths` or `paths-ignore`, while pull-request path filters may
remain. The release workflow source checker enforces that distinction. A docs-
only or packaging-only main commit must therefore produce fresh static,
script, workflow-security, and platform evidence at its own exact SHA instead
of inheriting an older successful run.

Workflows that combine branch, scheduled, and manual triggers include the
event name in their concurrency group. A newer run may supersede an older run
only for the same event and ref; a delayed scheduled or manual run must not
cancel exact-SHA push or pull-request evidence.

`Release Readiness` is a required lane for every pull request and `main` push.
It deliberately has no path filter. Its Ubuntu 20.04 container job installs the
release toolchain through the same versioned installer as the publication
workflow. That installer uses an isolated temporary GnuPG home, verifies the
Ubuntu Toolchain PPA's full fingerprint and Deb822 `Signed-By` boundary, and
checks amd64, glibc 2.31, and GCC/G++ major version 12. The job then installs
the exact CMake 3.31.6 wheel through pip hash-checking mode, binds GCC 12's
matching `gcc-ar`, `gcc-nm`, and `gcc-ranlib` wrappers, and performs the same
Release/LTO project build used by Linux packaging. Its Windows 2022 job
installs the digest-pinned UCRT64
compiler and runtime packages, configures the build with
`GNFS_ENABLE_NTL=OFF`, derives the four-DLL closure with `ldd`, launches the
packaged CLI with `/ucrt64/bin` removed from `PATH`, creates the deterministic
ZIP, and validates its package, license, DLL, and corresponding-source
manifest. These jobs are distinct from ordinary platform test rows: they prove
release distribution and toolchain boundaries rather than duplicating unit-
test selection.

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

- `test_joining_thread`
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
- `test_siqs_shadow_proof_runner`
- `test_siqs_shadow_proof_observe`

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

`test_relation_corpus_sha256` is the `instant` semantic-digest contract for
relation-corpus SHA-256 V1. Fixed vectors cover the defined zero-row digest,
one and two ordered rows, row-order sensitivity, every encoded relation field,
invalid relation shape, and terminal accumulator behavior. The encoding is
independent of host object layout and filesystem container bytes, so a worker
handoff can bind the decoded relation sequence reproducibly across platforms.

`test_ooc_durable_handoff` is split into two `instant` CTest entries. The core
suite fixes the canonical generic-handoff V1 and authorized-cleanup-marker V2
encodings, sealing, round-trip, zero-row, phase-kind separation, optional
duplicate-pending snapshot, and 64-KiB opaque-payload boundary. The negative
suite mutates every durable binding class and rejects malformed lengths,
versions, marker kinds, identities, extents, digests, truncation, trailing
bytes, and V1/V2 reinterpretation. This target is a pure protocol test: it
performs no filesystem mutation and grants no adoption or cleanup authority.

`test_ooc_cleanup_transaction` has five cross-platform CTest entries and three
additional macOS entries for the authorized V2 cleanup tail.
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
integration is covered separately by the observer, core, and private-lease
crash suites.

`OOCCleanupAuthorityObserver` is an `instant` raw-filesystem suite from the
same binary. It proves that exact canonical leaf spelling is required on every
platform. On macOS, it also replaces a byte-identical cleanup leaf after the
initial inventory and after all six logical reads, replaces the named private
directory at both boundaries, and supplies invalid hard-link metadata. It
exercises both cleanup-marker and generic-handoff inode replacement. Per-slot
handoff cases cover corrupt bytes, invalid canonical or pending link metadata,
wrong-context canonical bytes beside a valid preactive pending record, and two
individually valid but inconsistent records. The valid companion retains its
own exact fact while rejected leaves are foreign and a decoded pair conflict is
malformed; without the exact preactive reservation, a duplicate foreign
pending record remains foreign. A corrupt lease context beside already-foreign
handoff metadata, a known inventory replacement, or an exact byte conflict
also proves that diagnostic refinement cannot override foreign-first
precedence. The observer either reduces the changed inventory to foreign
evidence or rejects the directory ABA before returning. The macOS production
path reads all four
cleanup markers and both generic-handoff leaves through one held no-follow
directory handle, with complete 11-slot inventories before and after. Linux
and Windows retain the explicit path-limited adapter; nonempty deterministic
observation hooks are unsupported there. The returned raw facts contain no
handle or record snapshot, are accepted by no mutator, and are not an
authority permit.

The macOS-only `OOCAuthorizedV2CleanupCore` entry is an `instant` contract for
the cold authorized-cleanup executor. It covers direct canonical completion,
parent-durable absence evidence, the deliberately unspent pending-only
conversion boundary, and canonical-intent identity retention. Intent
reconciliation accepts only pending-only, canonical-only, or canonical plus
one exact duplicate pending record. Canonical confirmation failures retain the
fresh receipt; the duplicate-pending unlink boundary spends it and requires a
fresh cold receipt after any uncertain outcome. Failure-only hooks cover the
canonical confirmation and post-unlink directory-sync barriers, while
same-byte inode and metadata replacement tests prove zero additional mutation
before either spend boundary. Core also covers markerless and staged cold
tails, foreign inventory, and exact-successor drift injected at both the
unspent permit seam and a spent artifact seam. The two `fast` entries,
`OOCAuthorizedV2CleanupArtifactCrash` and
`OOCAuthorizedV2CleanupLeaseCrash`, partition the complete ordered fault-point
catalog. Each entry checks in-process interruption and also forks a child that
calls `_Exit` at every assigned durable boundary. The parent verifies the
resulting legal prefix and completes cold recovery with a fresh process-local
authorization receipt. Artifact recovery also exits after exact duplicate
pending removal but before its directory sync, then proves canonical-only cold
convergence. Together the entries cover all durable intent, handoff,
quarantine, staged publication, artifact unlink, owner, private-directory,
external `OWNED`, parent-sync, and final evidence boundaries. Other platforms
retain the explicit mutation-free unsupported contract until an equivalent
held-handle implementation exists. Replacement sandwiches are deterministic
checks inside the cooperating `BaseLock` domain; as elsewhere in the cleanup
protocol, an adversarial same-UID mutator racing the final snapshot and the
name-based `unlinkat` is outside the portable threat model.

The platform-limited metadata adapter is exercised directly for missing,
policy-compatible regular, invalid-mode POSIX, directory, hard-link, and
symlink leaves; it never reports `Exact`.
Those tests snapshot the complete test namespace and prove that role-correct
V2 records, malformed markers, foreign handoffs, exact handoff/V1 conflicts,
and partial V2 magic prefixes leave no namespace mutation. A dedicated C1
regression also proves that a rejected generic-handoff leaf retains
foreign-preservation precedence when legacy V1 markers coexist. Static entry
placement keeps the preflight before sync, rename, rewrite, reconciliation, or
unlink. The deferred-writer integration additionally snapshots an open pair
and V2 leaf around rejection, proving its fail-early cleanup-handoff preflight
runs before `finalize()` changes pair bytes. That observation never authorizes
publication across finalization.
The current runtime adapter combines all six logical leaf facts before
reduction. Recovery now receives a production-only, move-only,
`RecoverPrivateLease`-bound permit that retains the union observation and C1
witness through the complete action. Direct permit tests cover blocked
admission, wrong action and frozen paths, moved-from and repeated consumption,
C1 single consumption, and POSIX fork-child rejection. A public interruption
test proves that `RecoveryPermitAcquired` returns `Interrupted` without changing
the namespace and that a later recovery remains possible. A cross-platform
post-acquisition test inserts a previously absent handoff leaf and proves that
the macOS held-handle witness or Linux/Windows path-limited raw re-observation
rejects it without mutation beyond the injected namespace snapshot. The
stronger macOS-only regression replaces an exact pending handoff with a
byte-identical new inode and proves the same preservation result.
`RemovePrivateLease` has a separate permit and retained C1 consumer; its tests
prove its exact receipt generation is bound before C1 mutation, pre-mutation
interruption is namespace-neutral and retryable, a post-acquisition handoff
insertion is preserved on every platform, a macOS byte-identical pending
replacement is rejected, and a matching pending leaf is reconciled
successfully. An exact pending-to-rollback-tombstone fixture separately proves
that `remove_private_lease` returns `ForeignReplacementPreserved`, leaves its
receipt unspent, and changes no namespace leaf. The proof also binds both
external lease-pending siblings; tests cover pre-existing foreign siblings and
a post-acquisition byte-identical inode replacement before C1. Two macOS
stale-generation cases preserve the full new-generation snapshot for
pending-only and canonical-plus-duplicate C1 states. Malformed cleanup-marker
precedence remains `IntentCorrupt`, and only successful completion consumes
the lease receipt. The blocker-precedence test pairs that malformed marker
with deliberately mismatched generation inputs, so generation validation
cannot mask the retained union result. `RunLegacyCleanup` now has a separate
source-private permit for public begin and resume. Its first C1 consumer is
observation-only, and only an absent retained C1 state may advance to a
first-mutation gate. The gate revalidates the retained witness and binds every
authorization to the creator process, frozen paths, and exact `BaseLock`;
wrong consumers, cross-executor reuse, and failed revalidation are sticky.
Direct tests cover repeat use, wrong-action consumption, path/lock confusion,
and parent-authorized fork-child rejection. Public tests cover pre-mutation
interruption, post-permit and post-operation C1 insertion, exact intent/staged
pending markers, marker-rename failure and receipt retry, delete-authorized
and staged-only tails, empty-pair receipt commitment, unspent begin receipts,
and macOS byte-identical C1 replacement. The Linux and Windows policy branch
shares canonical/pending
begin and resume coverage. Nested Recover/Remove execution and the deferred
writer's publication-only path remain under their distinct action contracts.
Publication now mints a source-private permit only after the pair is durably
final. It binds the retained union, exact lease generation, exact finalized
pair, creator process, frozen paths, and a strong reference to the inherited
`BaseLock`. A per-lock logical-action claim rejects nested Remove on the same
live lock. Nested Recover opens a distinct lock and is rejected by the
existing OS lock. Both return `Busy` without minting a second permit.

The writer escrows its pair receipt for the complete publication attempt.
Tests observe it as unavailable in permit, pending, and canonical callbacks;
pre-canonical interruption restores it. The same exact durable canonical proof
commits the spend before duplicate-pending cleanup, final audit, and the
post-canonical callback. Later failures never restore the receipt. Receipt
extraction and reentrant publication are rejected during escrow.

The publication gate revalidates at pending preparation, binds the exact
durable pending inode before the pending callback, saves the exact successor,
and proves that only the expected intent slot changes before canonical rename.
After sticky canonical commitment, it revalidates the canonical successor
before duplicate-pending removal and successful return. Tests cover valid
staged sibling injection at pending and operation hooks, cross-platform
same-byte pending replacement at rename and unlink seams, duplicate-pending
unlink and parent-sync failures, nested actions, canonical interruption, claim
release, `DestinationExists` convergence, and a POSIX fork-copy publisher.
Every failure comparison uses a complete namespace snapshot.

Fresh private-lease construction now uses three independent action permits.
`ReservePrivateLease` is minted only after legacy recovery has finished and
released its permit. It retains exact marker and directory successors through
`RESERVED`, staging-owner, `OWNED`, and final-directory publication.
`ValidateFreshWriter` retains a separate permit across both `O_EXCL`
reservations, exact zero-byte pre-header states, paired V3 header validation,
and cleanup-receipt capture. The permit ends before
`ActivateFreshLease` is minted. Activation binds the exact preactive lease
generation and pair receipt, then treats durable `RESERVED` removal as the
sticky capability commit point.

The lease-crash suite interrupts all three permits before their first
mutation. Reservation and fresh-writer interruption preserve the complete
pre-action namespace and permit retry. Activation interruption occurs after
Fresh has ended, so it preserves the exact preactive pair for explicit
lease-receipt recovery rather than recreating Fresh rollback authority. The
suite also covers marker and directory phase prefixes, same-identity size
drift before header validation, post-phase foreign insertion, process
termination, and recovery of every retained prefix. Linux and Windows keep
the path-limited union observer and reject a present generic handoff as
unsupported before mutation.

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

The same binary freezes the source-private handoff-publication resume
authority. A raw `PrivateHandoffPublicationObservedPermitV1` cannot construct
`PrivateHandoffPublicationValidatedPermitV1` or enter reconciliation. The
move-only typed validator has no public constructor; validation consumes it on
every path, invokes it only on a fresh exact relation witness, and recaptures
that witness before minting the validated permit. macOS tests reject same-byte
pending replacement both before and after validation and reject tombstone
replacement after validation. They exercise the complete pending-to-tombstone
directory-sync matrix and terminate child reconcilers at every inner pair,
directory, and marker durability boundary. Each parent then proves fresh
`PendingRollback` acquisition, exact replay, and last-step tombstone removal.
Replacement and residual-marker cases preserve the attacked namespace without
permitting the next deletion.

Fork tests cover acquisition before and after process creation. The child
cannot use an inherited permit, clear the parent's logical action claim, or
reacquire through an inherited open-file-description. Resource-failure policy
also freezes the order in which a fully constructed permit state takes the
sole lock and then disarms the action-claim guard. Non-Apple tests require
`PlatformUnsupported` before lock creation or filesystem observation,
preserve a complete namespace snapshot, and then prove that a normal lock and
action claim can be acquired and released. They do not claim evidence for the
Apple cross-directory rename durability contract.

The macOS lease-crash branch also freezes consumed-canonical handoff adoption.
Only a validated permit whose reconciliation committed a complete canonical
terminal witness may enter the bridge. The returned reader retains the
original permit `State` and aliases its exact `BaseLock`; no descriptor
duplication, lock-path reopen, second lock object, or second logical-action
claim is permitted. Tests cover canonical-terminal and identical-dual
convergence, lock lifetime through reader destruction, unreconciled,
rolled-back, interrupted, failed, moved-from, and fork-inherited permits, and
a byte-identical canonical replacement immediately before receipt commit.
The last case must return `ForeignReplacementPreserved` without changing the
attacked namespace.

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

`test_distributed_sieve_worker_process` is the `instant` source-private
self-exec transport contract. A dedicated helper image proves exact argument
transfer, a prewritten bounded bootstrap frame on standard input, a report on
standard output, an empty environment, and closure of the batch's foreign
pipe endpoints plus the caller's explicit descriptor inventory. The parent
also covers partial `posix_spawn()` failure without invalidating earlier
tokens. Every batch result states `spawn_loop_entered` and
`child_set_complete` explicitly. A global validation or portability refusal
returns `false/false` with an empty child set. A staging or spawn-attribute
failure after result allocation returns `false/true` with a complete
zero-process slot set. A normal or slot-local partial spawn returns
`true/true` with the complete fixed-slot result set. Callers do not infer
either fact from vector size. Tokens are move-only, transfer the report
descriptor only after a confirmed terminal reap, cache the first non-`EINTR`
wait observation, and preserve uncertainty for stopped, mismatched, and
`ECHILD` observations. Windows exercises the explicit
unavailable result without claiming process-transport coverage.

The same binary covers the exact-role capability transport. It freezes child
descriptors `0..6` as bootstrap, report, standard-error snapshot, wave-root
directory, permanent WaveStore lock, attempt `BaseLock`, and anonymous
work-package file. Collision cases prove that the complete batch's child-side
sources are pre-staged at descriptor `7` or above before mapping. Closure
checks cover staged sources, foreign slots, and the platform close-from floor.
The generic spawn API remains authority-free. The repository policy checker
permits the exact-role source type and spawn entry point only in the
source-private process header, its implementation, the receipt-gated launcher
implementation, and this dedicated test. This binary does not claim
start-receipt consumption, WaveStore launcher integration, writer-authority
rehydration, or named-residue reconciliation.

`test_distributed_sieve_work_identity_codec` is the `instant` canonical
work-preimage contract. It compares the shared allocation-free field emitter
against an independent little-endian oracle, freezes the existing V1 work
digest, and proves decode/re-encode identity. Exact-prefix truncation,
trailing-byte, schema, tag, string/count bound, wire-ordinal, and semantic
corruption cases fail closed before the decoded value can be used.

`test_distributed_sieve_work_package_codec` is the `instant` immutable
work-envelope contract. It freezes the 80-byte little-endian header, exact
canonical identity body, and 32-byte domain-separated trailer against an
independent envelope oracle. Preparation and streaming emission recompute the
work binding on both passes; sink failure, size drift, or digest drift cannot
produce a trailer, and a longer second pass cannot write past the prepared
body boundary. Decoding proves the declared and actual extent plus both
digests before invoking the identity decoder, so integrity-unbound counts
cannot trigger destination allocation. Sink coverage includes partial
short-write prefixes at the header, body, and trailer boundaries.

On macOS and Linux, `test_distributed_sieve_worker_work_package_file` is the
`instant` anonymous
work-package capability contract. The injected operation matrix covers the
64KiB buffered writer, `EINTR`, short and zero writes, directory and file
policy drift, exact-once close diagnostics, fixed-leaf conflicts, and package
decode binding. Native macOS and Linux cases prove exclusive creation below a
borrowed owner-only directory descriptor, read-only same-inode reopen,
unlink-to-`nlink == 0`, directory synchronization, move-only token ownership,
and retained-descriptor revalidation. Windows does not build the native worker
entry, writer, or anonymous work-package capability graph. The repository
policy checker
permits the token and production factory only in their definition boundary and
this dedicated test, plus their first production composition inside the
receipt-gated launcher.

On macOS and Linux, `test_distributed_sieve_resume` is a split protocol and
WaveStore contract.
`DistributedSieveResumeCore` remains an `instant` pure record, dependency, and
compile-boundary test. It also compiles the source-private cleanup-authorization
passkey and receipt traits. The passkey and receipt have no production or test
mint route, so those checks freeze only the inaccessible capability surface
and are not evidence of cleanup authority.

The `DistributedSieveWaveStore*` CTest entries split the durable ownership
contract into ordered `fast` shards. The shards create and reopen the
source-private store, check exact manifest identity injection and bytes,
recover every durable publication prefix, reject noncanonical or symlinked
ancestor paths, detect ACL and namespace drift, and reject native-identity
replacement. Fork-and-pipe probes cover hook-time PID separation, concurrent
exclusion, and inherited-open-description lock lifetime. POSIX builds register
21 shards. Merge reservation, start publication, the three reconciliation
matrices, and cursor preparation use six separate shards so the combined
merge-start matrix does not exceed the `fast` single-run target. The first 20
shards use 60-second timeouts, and the process-launcher shard uses a 90-second
timeout. Windows excludes these native authority targets and instead runs
`DistributedSieveWaveStoreWindows` as an `instant`, 10-second file-level stub
contract. It proves that invalid shapes fail before the platform decision,
valid create/open and cleanup requests return `platform_unsupported` with no
result arm, hooks remain uncalled, and no requested namespace is created. The
same test target exits with the CTest convention code 77 on non-Windows hosts;
CTest and `scripts/test.sh` both record that result as a skip.

The shard union preserves the original WaveStore case order. CTest does not
register the aggregate because that would duplicate the complete matrix in
routine test selection. The compatibility selector `--suite wave-store` still
runs every WaveStore shard, and the no-argument binary still runs the core,
WaveStore, coordinator, and MergePrepared-protection suites. The script runner
continues to catalog that physical no-argument binary as `slow`, with a bounded
seven-minute timeout. CTest therefore reports the isolated fast shards, while
the script runner retains the single-process slow aggregate until it gains
logical suite aliases.

`DistributedSieveMergePreparedProtection` is a separate `fast` entry from the
same binary. It creates real finalized private OOC generations and exercises
both cold open and live root-claim classification for every legal prepared
publication prefix. Live claims remain read-only and return
`reconciliation_required`. Cold open rolls back an exact pending generation or
converges canonical and identical-dual publication prefixes, then opens the
strict manifest-bound store. The suite preserves the canonical
`MergeStartedV1` and all worker handoffs, proves that a pending rollback
advances to the next merge ordinal, proves that canonical prepared generations
do not expose a repeat ordinal, and repeats cold open with no mutation. A
bridge-hook lock probe must observe every canonical worker `BaseLock` as busy
while the target permit is held and must acquire it after the recovery round
releases locks. Malformed kind and predecessor bindings still fail closed with
complete wave-root and private-directory snapshots unchanged. The suite is
filesystem integration rather than an `instant` helper contract.

The M4b-P2b-P0b recovery contract extends that cold-open matrix. A ready
`DistributedSieveWaveStoreOpenResult` must satisfy a closed XOR: ordinary
reopen returns only `store`, while a terminal canonical prepared generation
returns only a valid `prepared_admission`. The terminal branch must retain the
permanent `WaveLock`, coordinator claim, manifest-ordered worker readers, and
merged target reader. Its release order is target, workers in reverse manifest
order, coordinator, then WaveStore. Fresh and recovered paths must expose the
same move-only `DistributedSieveMergePreparedAdmissionV1`, with no raw writer,
path, descriptor, cleanup receipt, or root-action capability.

`test_distributed_sieve_merge_writer` is the `instant` authority-free
manifest-order merge contract. It streams small finalized OOC fixtures through
the production first-`ABPair` writer, including an empty chunk and a valid
zero-row handoff, without constructing a WaveStore or private-lease
capability. The suite freezes first-payload preservation, a historical
packed-key collision, exact global and per-chunk receipts, and deterministic
output sequence and corpus digests. Same-count reader substitution,
sequence/corpus receipt drift, unsupported semantic versions, corrupt input,
and nonfresh output writers must fail before a result receipt can escape.
This test does not claim merge-generation finalization, prepared-record
publication, recovery, or cleanup authority.

On macOS, `test_distributed_sieve_merge_writer_authority` is the `fast`
real-filesystem transaction contract for a fresh merge generation. It consumes
a live coordinator admission once, creates only the exact deferred generation
writer, streams authenticated worker readers, finalizes through retained
handles, and publishes the typed `MergePreparedV1` payload. The suite covers
cross-chunk deduplication, the historical packed-key collision, zero-row
inputs, moved authority rejection, and real pending and canonical-promoted
publication prefixes. It also forks after the first relation remains in the
exact writer's stdio buffer: the child must fail on its next creator-process
check without flushing inherited bytes, while the parent still publishes the
exact expected corpus. On other platforms, the binary performs compile-time
API contract checks and skips the runtime transaction; production returns
`platform_unsupported` before minting or namespace mutation. Live claims must
protect interrupted typed prefixes with `reconciliation_required`. Cold open
now rolls back exact pending prepared prefixes and converges canonical or
identical-dual prefixes before ordinary manifest validation.
The relation-layer lease-crash suite also covers the lvalue transactional
reader adoption used by recovered prepared admission. It requires three trusted
aggregate callbacks: after the terminal match, inside the relation exact ->
callback -> exact receipt sandwich, and after reader construction under a final
exact sandwich. Callback rejection or a late injected interruption must leave
the consumed permit held, keep a competing independent adoption busy, and allow
an exact retry through that same permit. Fork-inherited and moved-from authority
must remain unusable without weakening the parent lifetime.

The MergePrepared protection shard exercises the aggregate boundary in the
real WaveStore path. Same-byte `MergeStartedV1` replacement immediately before
the target's final reader callback must fail as `namespace_conflict`. An
interruption at a later worker's receipt callback must observe every target and
worker lock held, unwind every partially adopted reader, leave no next-merge
branch, and permit an exact hook-free retry.

This evidence closes M4b-P2b-P0b. M4b-P2b-P1 adds a second recovery boundary
to `test_distributed_sieve_merge_writer_authority`. Cold open must observe four
real no-handoff writer residues without mutating them: empty, partial, and
complete `INCOMPLETE` pairs, plus a `FINAL` pair whose typed payload was built
before handoff publication. Explicit merge preparation then rolls the exact
latest canonical `MergeStartedV1` lease back to P0 while preserving the start
record, worker inputs, and absent successor namespace.

The same binary interrupts all eight raw-recovery boundaries from action-permit
admission through durable `OWNED` removal. Each prefix must cold-reopen and
converge to P0. Same-byte index and data inode replacements must fail as
`namespace_conflict` without deleting either the replacement or displaced
object. A competing process must observe `lock_busy`; after it exits, cold
reopen and recovery must succeed. This authority binary stops at the common
prepared admission. The separately cataloged merge-commit test owns every
positive `WaveMergeCommitV1` consumer claim.

`test_distributed_sieve_wave_merge_commit` is the dedicated real-filesystem
contract for the first `WaveMergeCommitV1` consumer slice. On macOS, its `core`
suite first commits a fresh prepared admission, then byte-compares that commit
with a cold reopen of the resulting committed tail. A separate case cold-opens
and consumes a canonical prepared admission. Both paths use a three-slot wave
containing an ordinary handoff, a zero-row handoff, and an empty chunk, and
prove that commit publication never removes worker artifacts. The suite also
rejects a moved admission replay without rewriting the canonical commit. The
`commit-crash` suite re-executes after pending durability, canonical promotion,
and canonical durability, then requires cold recovery to reopen the exact same
commit and merged corpus without repeating merge work. The `protection` suite
replaces the canonical commit with a byte-identical new inode between its first
and final successor validation and requires fail-closed rejection. It repeats
the replacement after a committed-tail admission exists and requires that live
tail to become invalid. The suite also rejects a competing permanent wave-lock
owner without mutation. Unsupported platforms register only the `platform`
suite.
That suite freezes the inaccessible admission and committed-tail type surface
and confirms that its compile-time-only probe leaves a candidate namespace
absent. It does not forge a prepared admission or call the consumer, so this
suite makes no runtime consumer claim. The repository policy checker separately
freezes the consumer order: admission validation, the non-Apple pre-mutation
return, then Apple-only context construction and publication.

The macOS suites start as `slow` entries until isolated Debug measurements
justify a narrower tier. Their timeouts bound each logical shard rather than
classify its expected duration. CTest does not register the aggregate because
that would duplicate the complete matrix. The script runner catalogs the
physical no-argument binary as `slow` with a five-minute timeout; the aggregate
runs all supported-host shards for direct changed-file validation.

`test_distributed_sieve_resume` is also the dedicated M2j-A receipt-gated
launcher contract.
macOS and supported Linux/glibc hosts run its positive launcher matrix. Every
non-Windows host runs the close-all-unavailable case: supported hosts use the
trusted force-unavailable hook, while musl, older glibc, and other unsupported
hosts exercise the real capability query. The source-private WaveStore member
derives each bootstrap frame from the canonical `AttemptStartedV1` record
rather than accepting caller-supplied bytes. It reruns live bound-work
validation, consumes the start receipt, creates and unlinks the immutable
package, revalidates the receipt, and supplies the exact descriptor `3..6`
capability set before the first child can start. The launcher rejects a host
without an atomic close-all spawn primitive before process preparation.

All result storage, receipt anchors, bootstrap and argument views, package
storage, capability storage, and transport staging complete before the first
spawn. They do not all precede the one-shot receipt gate. A failure after that
gate can therefore return `armed_no_child` while still proving that it started
zero children. Once the lower transport enters its spawn loop, a later slot
may fail after earlier slots have started, and the result reports that partial
batch without discarding successful composites.

The launcher consumes the transport's explicit spawn-loop and complete-child-
set facts. A closed global refusal and a complete zero-process slot set remain
proven zero-child outcomes. A complete, internally consistent per-slot set
maps to `armed_no_child`, `partial`, or `all` by its process count. Any entered
or otherwise ambiguous result without such a set maps to `indeterminate` and
quarantines every receipt rather than guessing which `BaseLock` may protect a
live child.

Ten launcher cases freeze the current boundary. The non-Windows unavailable
case proves `failed_before_gate`, `process_preparation`, zero spawn, normal
receipt release, and a ready store. The other nine are successful self-exec,
live-binding mismatch before the receipt gate, invalid initial receipt after
the gate, post-carrier fixed-leaf residue, a later-slot hook rebuilding an
earlier slot's fixed leaf, post-carrier directory replacement, second-slot
namespace conflict, three-slot partial spawn, and abandoned composite
quarantine. Those nine are registered and executed only when the host reports
atomic fixed-capability close-all support; unsupported non-Windows hosts do
not execute nine no-op cases. The happy and partial children decode the
canonical `AttemptStartedV1` bootstrap, bind descriptor `3` to the exact
manifest, and bind descriptor `6` to the expected work digest. They also
verify all four capability descriptors, same-open-file-description lock
inheritance, the anonymous package's `0400` mode and zero link count, and
absence of descriptor `7` or above. The partial case keeps the successful
slots' `BaseLock` objects busy and releases the failed slot's lock.

The quarantine case runs in a forked supervisor. Its self-exec child validates
the launch, closes and proves descriptor `5` absent, emits one report frame,
and remains alive. Destroying the unreaped composite does not wait or kill;
it deliberately quarantines the receipt, WaveStore state, and `BaseLock` for
the rest of the supervisor process. The lock remains busy both while the child
is alive and after an external terminal reap. The supervisor boundary prevents
that intentional leak from accumulating in the test process.

Carrier diagnostics with `named_may_remain` stop the batch and set
`reconciliation_required`; the launcher never blindly unlinks the fixed leaf.
After carrier success, one batch-wide absence gate remains armed across every
slot. It is released only after the complete receipt set, every exact held
attempt-directory binding, and every fixed package leaf's absence have all
been revalidated after the last carrier hook. Any earlier return sets the same
flag while preserving the primary WaveStore status. The cross-slot case
rebuilds slot 0's fixed leaf from slot 1's hook and proves that the final gate
reports slot 0, starts zero children, and preserves the residue. The
directory-replacement case proves that the launcher retains both the displaced
original and the replacement directory.

The launcher boundary is internal and test-only. It does not authenticate the
path-based `posix_spawn()` executable as the same object bound by the
manifest, run the sieve, publish a no-delete handoff, or delete a named package
residue after restart. The policy checker admits launcher composition only
inside the exact `launch_worker_process_batch_v1()` function body, with closed
counts for bound work, carrier, package reader, and fixed-capability uses.
Direct launcher test use remains confined to `test_distributed_sieve_resume`
and the worker-entry self-exec contract described below.

The same binary covers the M2j-B B1 read-only restart classifier and B2
identity-bound reconciliation. A canonical `0400`, single-link package residue
can reopen only inside an exact final P8 directory whose owner and owned
markers are canonical. Its `AttemptStartedV1` must be canonical-only, must be
the latest attempt for that chunk, and must bind the same manifest, lease ID,
owner marker, directory, relative stem, and attempt coordinate. The inspector
decodes the complete work identity and authenticates body extent, total
extent, work digest, package digest, native file identity, file extent, and
effective owner. WaveStore stores the compact witness in both inventory
observations.

The positive case first reopens the unchanged no-residue P8 baseline. It then
writes a package through the canonical codec, seals and synchronizes the fixed
leaf, destroys the store, and proves that `open()`, `revalidate()`, the generic
root claim, and the exact attempt claim all succeed after restart. Legacy
private-lease recovery and fresh same-chunk creation still return
`reconciliation_required` without mutation. The sole authorized route is
`reconcile_worker_attempt_started()`: while its attempt `BaseLock` is held, it
revalidates the exact claim, record, directory, and compact witness; expands
the full carrier witness; removes that exact residue or confirms its absence;
and only after a confirmed residue-free successor normalizes the record and
recovers the private lease to P0.

The carrier synchronizes the held attempt directory for both the
residue-present and already-absent paths. Interruptions after unlink and after
directory durability leave the record and P8 lease unchanged; destroying and
reopening the store then replays both prefixes to P0. Selected directory-sync
failures return `durability_failed`, preserve the residue-free successor, and
also replay to P0. Package, directory, record, and `BaseLock` mutation
sandwiches fail closed before later record or lease mutation. A package
inserted after the first residue-free observation is preserved rather than
deleted, and hook-time probes prove that the attempt lock remains held.

Negative matrices preserve every observed leaf. They cover no record, pending
only, identical dual, wrong-coordinate, and historical-attempt bindings;
`0600`, truncated, extended, package-corrupt, valid-wrong-work, symlink,
directory, and hard-linked leaves; and same-byte package, attempt-directory,
record, and `BaseLock` replacements. Publication and normalization hooks add a
valid package immediately before record mutation and prove that full claim
revalidation returns `namespace_conflict` before the publisher runs. A
cross-chunk P3 recovery hook replaces another chunk's package and proves that
the staging directory is not removed.

B1 itself grants no cleanup, launch, or writer authority; B2 adds only the
closed reconciliation transition above. Native cleanup is exercised on macOS
and Linux. Unsupported platforms fail with the explicit platform status, and
the portable threat model still excludes an adversarial same-UID namespace
mutator. The policy checker permits one direct production inspector call in
each of `validate_private_lease_attempt_inventory()` and
`reconcile_worker_attempt_started()`, and one direct production reconciler
call only in the latter. It counts all identifier uses to reject aliases and
duplicates, forbids both `_with_ops` seams in WaveStore, confines fixed-leaf
unlink authority to the carrier, and keeps the legacy runner, launcher,
pipeline, and relation paths isolated.

B2 itself does not grant worker execution or publication authority. The M3a-1
entry boundary below rehydrates descriptors `3..6` into a read-only typed
token, M3a-2a consumes that token into exact-directory append/finalize
authority, and M3a-2b publishes the no-delete handoff. M3a-2c.1 performs one
exact durable worker chunk through those capabilities. M3a-2c.2A now
reconciles exact worker-handoff publication prefixes during a cold
`DistributedSieveWaveStore::open()`. Only missing-only coordination and
adopted/executed disposition reporting remain in M3a-2c.2.

`test_distributed_sieve_worker_entry` is the `fast` source-private M3a-1
exec-image rehydration contract. On supported macOS and Linux hosts, the
self-exec child consumes the canonical `AttemptStartedV1` bootstrap and fixed
descriptors `3..6` exactly once. It stages retained capabilities at descriptor
`7` or above with `FD_CLOEXEC`, closes the fixed inputs, and returns only a
move-only, current-process token with immutable record, manifest, work,
chunk, and package-witness facts. The token exposes no raw descriptor, path,
writer, cleanup, handoff, retry, or completion authority.

Admission recovers and no-follow reopens the exact absolute root, revalidates
its direct-parent policy, and binds the root and manifest identities. It reads
the complete canonical `AttemptStartedV1` predecessor chain, rejects pending,
later, and over-budget attempt residue, then binds the permanent and attempt
`BaseLock` names, final P8 directory, exact `RESERVED`/`OWNER`/`OWNED` markers,
frozen base-path digest, absence of every same-attempt staging candidate, and
the anonymous read-only `0400`, zero-link work package. Each lock uses a
contender-first check followed by retained-descriptor validation, so an
unlocked or freshly reopened same-inode descriptor cannot acquire a lock and
masquerade as the inherited open-file-description. A complete second
validation closes replacement sandwiches before the token becomes usable.

The test matrix covers the happy binding, revalidation, repeated adoption,
fork invalidation with parent-token preservation, every missing fixed
descriptor, package policy drift, fresh-open permanent and attempt locks,
forged predecessor chains, wrong but internally consistent base-path markers,
foreign staging residue, direct-parent policy drift, same-byte manifest,
attempt-record, and `BaseLock` replacement, and private lease marker or
pending-leaf drift. Success and failure paths both prove that standard input
and descriptors `3..6` are closed. Every case preserves the P8 artifacts and
confirms that the entry boundary creates no relation pair, handoff, cleanup
intent, or cleanup authorization. Unsupported platforms exercise the entry
API directly and return an explicit platform status without claiming native
coverage.

`test_distributed_sieve_worker_writer_authority` is the `fast`, source-private
M3a-2a/M3a-2b capability-conversion and terminal-handoff contract. It consumes
a valid M3a-1 token exactly once, adopts the inherited attempt `BaseLock` as the
same open-file-description, and creates only `corpus.relidx` and
`corpus.reldata` relative to the retained P8 directory handle. The narrow
authority is move-only and process-bound. It exposes immutable worker facts,
append, count, and one typed terminal handoff operation; it does not expose a
path, descriptor, store ID, raw `OOCRelationWriter`, generic handoff publisher,
cleanup receipt, cleanup intent, or deletion operation.

The native happy path writes two fixed relations, rejects a post-fork child's
mutators after inheriting a nonempty stdio buffer while preserving the parent
authority, finalizes the exact pair, recomputes its sequence receipt and
semantic corpus SHA-256 through duplicated exact handles, and publishes a
sealed `WorkerHandoffV1`. The test adopts the canonical handoff under the same
private lock, replaces both named files with corrupt same-extent objects,
proves a path-based reader rejects them, and still reads the original pair
through the frozen native handles. A zero-row, nonempty-chunk case fixes the
defined empty digest, exact progress facts, and header-only extents. A
pre-cache allocation failure proves strong retry-cache atomicity. Each of the
four generic publication durability prefixes is injected independently and
its pending/canonical/`RESERVED` shape is observed before retry. A different
but valid completion is rejected without changing the complete namespace
snapshot, while an exact retry converges the same canonical handoff without
creating cleanup intent.

Replacement sandwiches for the P8 directory, attempt `BaseLock`, and
private-lease markers fail after the second validation; post-conversion lock or
marker drift is rejected again at the next append, while post-conversion P8
replacement is rejected independently by the terminal preflight. Wrong
base-path binding and foreign staging residue also fail closed. A failed or
successful conversion burns the entry. Injected fresh-construction failures
preserve the primary cancellation error, distinguish a clean rollback from a
foreign replacement that requires reconciliation, and prove exact-identity
rollback cannot remove that replacement. macOS exercises authoritative
handoff publication and adoption. Linux reaches writer authority but rejects
terminal handoff before finalization or state mutation, preserving append
authority and every namespace leaf. Windows and other unsupported hosts reject
the earlier worker-entry platform gate. Neither fallback claims native
publication coverage.

`test_distributed_sieve_worker_execution` is the `fast`, source-private
M3a-2c.1 runtime-rehydration and real-chunk contract. Its 180-second timeout
covers a bounded real lattice-sieve/cofactorization probe and a self-exec
executable-digest check; it is deterministic and contains no benchmark or
unbounded search. The test rehydrates all canonical execution settings,
reconstructs the polynomial and factor base, and verifies a three-special-Q,
12-relation result twice by row order, sequence receipt, and semantic corpus
digest. It also covers special-Q and soft-relation cap priority, admission
rejection and deduplication rollback, sink failure, projective cursor
normalization, and a resumable projective hole.

The paired `test_distributed_sieve_worker_writer_authority` self-exec case
drives the complete entry-to-writer-to-handoff facade for both zero-row and
positive-row chunks. On macOS it reopens the WaveStore, adopts frozen native
OOC handles, and reads a 72-row corpus produced from one affine special-Q of a
nondegenerate cubic polynomial. Every row is checked independently for the
rational value, algebraic norm, exact factor products, prime-ideal roots,
special-Q divisibility and recording, coprimality, and admission invariants
before both receipts are reproduced. A launch through a symlink hashes the
launched executable bytes. Linux reaches the documented terminal-handoff
unsupported boundary before corpus mutation; Windows and other unsupported
hosts stop at worker entry. Those fallback cases assert fail-early behavior
and do not claim native end-to-end coverage.

`test_distributed_sieve_worker_writer_authority` is the authoritative
writer-to-cold-open entry contract for M3a-2c.2A on macOS. Real writer children
terminate with `_exit()` at pending and canonical publication boundaries. A
fresh WaveStore arms the exact pending handoff as a wave-root rollback
tombstone or converges a canonical or identical-dual prefix. Missing
`RESERVED`, nonidentical duals, pair identity or extent mismatches, and
same-byte pending or canonical replacements remain preserved. A live attempt
`BaseLock` blocks cold open without mutation until release.

`test_ooc_cleanup_transaction` owns the pending-to-tombstone directory-sync
matrix, every inner pair, owner, directory, and marker crash prefix, residual
marker rejection, and same-byte tombstone replacement. It retains the
tombstone through every partial rollback state. The fresh-open tests below own
wrong `AttemptStartedV1` digests, same-byte attempt-record replacement, and
partial-tombstone reopen. Together the three fast binaries prove that only the
matching preactive pair and lease can roll back, while a canonical handoff
revokes only its exact `RESERVED` generation.

`test_distributed_sieve_resume` freezes the complementary live-store and
multi-prefix rules. `revalidate()` observes a stable legal publication prefix
twice, releases all temporary permits, and changes no namespace leaf. It
returns `reconciliation_required` only for a prefix that still needs
mutation. A worker-handoff canonical prefix is ready, but every
chunk-terminal-failure prefix remains `terminal_failure_pending` until the
same-lock typed publisher confirms its durability. Cold `open()` is the
authorized convergence path. It acquires terminal and recoverable attempt
`BaseLock` objects in ascending manifest-chunk and attempt order, retains the
entire set while reconciling the highest nonterminal prefix, releases in LIFO
order, and rebuilds the complete global attempt chain before the next round.
The dual-chunk case observes both locks as busy in each round, observes both
as reacquirable at the round boundary, replaces a lower pending record with a
same-byte new inode, and proves that only a fresh next-round observation can
continue.

`DistributedSieveWorkerCoordinator` is the `fast`, 180-second M3a-2c.2
execution and bounded-retry ownership contract. The coordinator consumes one
WaveStore, retains its whole-round claim, and derives chunk and attempt
coordinates from the frozen manifest. Before reconciling or reserving an
attempt, one pure
`bind_distributed_sieve_work_v1()` call validates the supplied runtime objects
against the manifest work digest. Production reaches process creation only
through one source-private call site to the sealed
`DistributedSieveWaveStore::launch_worker_process_batch_v1()` member. It then
waits through typed launched-attempt owners and adopts terminal corpora through
same-handle readers. The coordinator never receives raw process, cleanup,
unlink, or legacy distributed-sieve authority. Its result owns the WaveStore,
round claim, and readers in close-only destruction order.

The positive coordinator matrix is:

| Scenario | Launch oracle | Manifest-ordered result | Artifact oracle |
|---|---|---|---|
| Fresh wave | One sealed batch contains every nonempty missing chunk | Every launched terminal chunk is `executed` | Every worker handoff remains |
| All handoffs present | Zero launch calls at runtime | Every nonempty chunk is `adopted` | Existing handoffs remain byte-identical |
| Mixed wave | The batch contains only exact missing nonempty chunks | Existing chunks are `adopted`; new terminal chunks are `executed` | Both old and new handoffs remain |
| More chunks than special-Q values | Zero-length chunks never enter the launch batch | Each zero-length chunk is `empty` in its manifest slot | No empty-chunk artifact is created |
| Nonempty zero-row handoff | Zero relaunches for that chunk | The terminal chunk is `adopted` with a zero-row receipt | The zero-row handoff remains readable |
| Failed attempt after cold reopen | The exact quiescent `AttemptStartedV1` is reconciled before one successor enters the batch | The successor is `executed` at ordinal `k+1` | The successor predecessor is the reconciled `A_k` digest |
| Live attempt `BaseLock` | Zero launch calls and no successor namespace | The round returns `retry_busy` for the exact manifest slot | The busy attempt is unchanged; earlier manifest-order attempts may already be normalized idempotently |
| Handoff published after initial observation | The second durable observation converts the exact `A_k` to adoption before any reconcile or launch | The chunk is `adopted` | No `reconciled_attempt` fact or `A_{k+1}` namespace exists |
| Handoff published after retry observation | The typed reconciler observes the terminal witness while holding the exact old-attempt `BaseLock` | Expected same-handle adoption replaces retry | Native marker and artifact snapshots still match; no successor exists |
| Retry budget exhausted | Reconciliation retains the final `BaseLock`, publishes or confirms the terminal record, and starts no chunk, including otherwise missing chunks | The round returns `retry_exhausted` with one `terminal_failed` disposition | Reopening reacquires the exact final admission and confirms the canonical record without launch or adoption |

The launch ledger must match every `executed` result by chunk, attempt ordinal,
lease, and the complete `AttemptStartedV1` record. A successful wait or child
report without the matching durable handoff is a failure, not `executed`.
The busy-late-handoff case uses an explicit process handshake instead of a
timing delay. The self-exec worker blocks `SIGUSR1`, adopts the exact entry and
its `BaseLock`, reports readiness, and waits. The coordinator's busy-observation
hook releases it; a successful terminal wait and exact handoff adoption then
prove publication. Exceptional teardown kills and reaps a still-waiting child.
The child also arms a parent-liveness guard before reporting readiness: Darwin
uses `EVFILT_PROC` and Linux uses `PR_SET_PDEATHSIG`, so an externally killed or
timed-out test process cannot orphan the still-waiting controlled worker or its
`BaseLock`.
Child exit alone never grants retry authority. A retry requires a fresh
durable observation, nonblocking acquisition of the exact old-attempt
`BaseLock`, and `reconcile_worker_attempt_started()` returning either the
immutable record plus its manifest-bounded next ordinal or an exact terminal
handoff witness. The terminal witness includes native marker and artifact
snapshots and can be consumed only by expected same-handle adoption. The
result's `reconciled_attempt` is read-only evidence and grants no launch,
cleanup, or publication capability.

Inventory validation performed while an attempt `BaseLock` is already held
must remain in that open-file-description domain. The source-private relation
bridge accepts one non-exportable borrowed-lock token, validates the parent,
named lock, retained descriptor, identity, ownership, and mode, and duplicates
the descriptor with `F_DUPFD_CLOEXEC`. The adoption receipt receives that
same-open-file-description duplicate and releases it by `close()` only; it
must neither acquire a second independent `flock` nor issue `LOCK_UN`.

Attempt claim permits at most one exact incomplete-to-terminal refresh. A
pre-lock observation may consume it, or the first inventory witness after
target-lock acquisition may consume it. After the post-lock fault boundary,
authority is revalidated and a second held inventory witness must match the
accepted successor exactly; it cannot refresh the baseline again. The
coordinator matrix includes same-byte terminal replacement after target-lock
acquisition and requires `namespace_conflict`, so inode replacement cannot be
laundered as the allowed terminal transition.

The policy checker keeps the coordinator API source-private, count-closes the
single exact attempt-open call, typed reconciler call, and sealed launcher call
site, and fixes their order after bound-work validation. It rejects the generic
lease-recovery shortcut, premature `ChunkTerminalFailureV1` use, lower
transport, cleanup, unlink, legacy entry, or public-header use. Its
`DistributedSievePolicyInventory` CTest is classified as `slow` because the
closed repository scan and self-test matrix can exceed 15 minutes on shared CI
runners and therefore exceed the `fast` budget.

`retry_exhausted` remains the coordinator-round diagnostic, while its
`terminal_failed` chunk disposition carries the durable terminal-wave
evidence. The typed WaveStore publisher retains the reconciler's root claim
and same-open-file-description final-attempt `BaseLock` after P0, then
consumes that move-only admission. It accepts no caller-built terminal record,
reason, digest, confirmation boolean, coordinate, or range. The publisher
internally emits only `attempt_budget_exhausted` with unavailable wait facts.

The canonical root leaf is
`.gnfs-wave-v1.chunk-terminal-failure-cCC`; the pending leaf appends
`.pending`. The wave permits at most one terminal coordinate. Pending-only and
canonical-only prefixes, as well as exact same-coordinate dual prefixes,
require the same retained-admission normalizer. Raw observation treats every
such prefix as structurally valid but unconfirmed because a canonical rename
may be visible before its parent-directory durability barrier. The coordinator
gives the prefix whole-wave priority and stops before process launch or
handoff adoption, but reports the absorbing terminal state only after the
publisher creates, recovers, or confirms the canonical record durably.

The acceptance matrix is:

| Scenario | Required oracle |
|---|---|
| Pending-only recovery | The retained-admission normalizer promotes the exact existing bytes; it does not recompute reason or digest |
| Exact canonical/pending dual | The same normalizer confirms the canonical identity and removes only the exact redundant pending identity |
| Same-byte terminal inode replacement | Replacement after the accepted held witness returns `namespace_conflict`; it cannot refresh the admission baseline |
| Late canonical handoff | Handoff wins under the retained final-attempt `BaseLock`; no terminal leaf is published |
| Busy final-attempt `BaseLock` | Coordinator recovery returns `retry_busy` before publication with zero terminal namespace mutation |
| Second terminal coordinate | Inventory validation preserves both leaves and returns `namespace_conflict` |
| Canonical-only terminal reopen | The coordinator reacquires the exact final admission, confirms the existing record, and returns the terminal wave error with zero launch and zero handoff-adoption calls |
| Initial reason normalization | The sealed record contains `attempt_budget_exhausted` and unavailable wait facts, independent of caller diagnostics |

Same-handle adoption also sandwiches the full corpus read with final exact
checks for both the nested `OWNER` marker and root-level `OWNED` marker. The
coordinator suite replaces each marker with a byte-identical new inode at the
pre-commit boundary and requires `namespace_conflict`; semantic record equality
alone is not sufficient lease authority.

The typed worker envelope must bind the exact canonical attempt digest, lease,
chunk, and ordinal before the relation layer can upgrade an observed permit.
The dedicated generation-bound outer executor retains the tombstone and exact
marker tail but delegates pair, owner, and directory mutation to the shared
preactive rollback core. The policy checker freezes that core, its
owner/index/data-only scanner, the two branches in
`recover_owned_private_lease_locked`, its sole typed call site, and a
repository-wide default deny for other callers. The complete generic recovery
core is exact-frozen so an earlier terminal return cannot bypass validation,
rollback, or marker-tail cleanup. The checker also freezes the complete
code-token bodies of the ordinary `recover_private_lease` and
`remove_private_lease` scopes and rejects raw physical-line splices before
comment or literal masking. Both scopes must pass cleanup-union admission
before the generic executor can run, and a rollback tombstone remains foreign.
Generic and typed rollback use the same preactive fault-point matrix. The
typed runtime matrix inserts both an unknown child and a syntactically valid
cleanup intent after tombstone durability; both must fail before the shared
core's first rename and preserve the complete injected snapshot.
At each relation mutation boundary, the WaveStore bridge revalidates the held
root, permanent lock, manifest, all exact attempt records, ordinary lease
witnesses, and non-current retained handoff permits. Phase-specific inventory
projection removes no unrelated pending, staging, or other-attempt leaf. Root,
lock, manifest, current-attempt, and lower-attempt replacement matrices prove
that the bridge diagnostic takes precedence over a generic interruption and
that no following relation mutation occurs. Non-Apple hosts fail resume
acquisition before lock or filesystem observation and do not claim native
convergence or rename-durability coverage.

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
