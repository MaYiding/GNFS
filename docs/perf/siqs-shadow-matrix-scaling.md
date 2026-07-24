# SIQS Shadow Matrix Scaling

## Scope

This document records the scaling evidence and promotion gates for the staged
Self-Initializing Quadratic Sieve (SIQS) shadow matrix. It covers the
deterministic dense solver in
`include/gnfs/siqs/shadow_matrix.hpp`. It does not claim production
two-large-prime (2LP) yield or authorize routing a shadow factor result from
`factor()`.

`include/gnfs/siqs/shadow_proof_runner.hpp` provides a production-facing
read-only facade. `GNFS_SIQS_SHADOW_PROOF=observe` now invokes it after sieve
worker join and before the legacy merge; every terminal outcome continues into
the legacy merge, solve, and extraction path. Its default admission envelope is
32768 raw relations, 64MiB of portable logical payload, 16384 graph edges, 4096
cycles, 262144 total cycle incidences, 4096 row candidates, and 4096 pretrim
rows. The facade also preserves the solver's independent dense byte and
variable limits. All limits are inclusive; the next object returns a typed
fallback instead of allocating beyond the admitted envelope.

The facade keeps the caller's raw relations intact so the observe seam can
continue into the current merge and solver. Consequently, its production peak
includes raw storage, owning assembly rows, and the packed matrix at the same
time. The emitted before/after process-memory snapshots are endpoint and
lifetime-high-water observations, not an isolated transient shadow peak.
Allocator retention can also keep the after snapshot high. Only fresh-process
lifetime comparisons are interpretable. The 256-A proof executable releases
raw storage before solving, so its measured peak is correctness evidence, not
a production RSS projection.

The production-overlap probe uses the current 1LP collector and the actual
`factor()` observe seam. It is a measurement contract, not a routing mode. Its
comparison runs one `off` control and three independent `observe` samples in
fresh processes. It validates every sample separately and does not require raw
corpus or fingerprint identity across processes. Production sieve completion
is schedule-sensitive, so such an identity requirement would reject valid
measurements without strengthening the proof or RSS contract.

The immediate safety decision is:

1. Validate shared modulus and factor-base context once per solve.
2. Reject dense shapes that exceed the explicit backend or memory boundary.
3. Optimize worker lifetime only after the safety boundary is enforced.
4. Add a direct sparse backend only after it has typed failure and dependency
   verification contracts.
5. Keep production 2LP disabled until bounded live-sieve evidence passes.

## Measurement Method

The local measurements used an Apple M5 with four performance cores, six
efficiency cores, and 24GiB RAM. The benchmark used Homebrew Clang 22.1.6 with
`-O3 -DNDEBUG -std=c++20 -mcpu=native`. The solver and elimination measurements
were taken while developing revision `0baa518`; the shared-validation
measurements use revision `b3aeb67`. Revision `9d95503` preserves the equivalent
corpus, measurement scopes, safety gates, and structured output in
`tests/test_siqs_shadow_matrix_bench.cpp`. Revision `a5c127a` adds the production
persistent-worker implementation, its three-way kernel comparison, and the
ThreadSanitizer (TSan) gate.

The project runner always builds the current tree in Release mode, rejects
`--no-build` and `--retry`, and does not assert timing thresholds. It prints the
seed, shape, CMake build type, warmup and repetition counts, min/median/max wall
time, and result digest.
`std::chrono::steady_clock` encloses only the selected operation:

| Mode | Timed scope |
|---|---|
| `solve` | Complete public solver call, including row checks, packed allocation, elimination, and dependency extraction |
| `kernel` | Matrix reset, pivot search, and elimination; corpus and worker-team construction are excluded |
| `prepare` | Public or prevalidated row-identity pass |
| `fbcheck` | The requested number of full factor-base scans |

Factor-base generation, row construction, the initial corpus validity check,
and post-run digest calculation are outside every timed scope. The full-solver
and three-way kernel 50-digit comparisons use two warmups and five measured
runs; their 70-digit comparisons use two warmups and three measured runs. The
crossover replay and 90-digit-shaped validation commands use two warmups and
three measured runs.

The fixed seed is `0x53a9f19d97e8c641`. The elimination corpus isolates matrix
costs:

- slot zero is the sign sentinel, followed by genuine ascending primes;
- each row has 20 columns for distinct odd primes chosen by SplitMix64;
- the modulus is 2 and `x_modulus` is 1, so every row passes exact identity
  validation;
- row IDs are unique and the dependency digest is fixed across worker counts.

The synthetic timing shapes use total equation counts of 1600, 15000, and
130000. Those counts equal the primary factor-base sizes in `select_params`, so
they are one column smaller than live spans that also contain the sign
sentinel. Their row counts add 100 to the synthetic equation count. These
dimensions must not be presented as live production shapes.

This corpus does not model live SIQS row weights or large-prime distributions.
The constructed cross-size oracle in
`tests/test_siqs_shadow_cross_size.cpp` separately checks arithmetic,
provenance, fingerprints, dependencies, and factor extraction at the live
50-, 70-, and 90-digit factor-base column counts.

The following commands reproduce the recorded scopes. Timing values can drift
with thermal state, scheduler behavior, compiler revision, and later source
changes; digest equality and the structured schema are the correctness gates.

```bash
./scripts/test.sh bench-siqs-shadow solve --fb 1600 --rows 1700 --weight 20 --workers 1,2,4 --parallel-threshold 0 --warmups 2 --reps 5
./scripts/test.sh bench-siqs-shadow solve --fb 15000 --rows 15100 --weight 20 --workers 1,2,4 --parallel-threshold 0 --warmups 2 --reps 3

./scripts/test.sh bench-siqs-shadow kernel --fb 1600 --rows 1700 --weight 20 --workers 1,2,4 --parallel-threshold 0 --warmups 2 --reps 5
./scripts/test.sh bench-siqs-shadow kernel --fb 15000 --rows 15100 --weight 20 --workers 2,4 --parallel-threshold 0 --warmups 2 --reps 3

for fb in 2500 4000 5500 8000 10000; do
  rows=$((fb + 100))
  ./scripts/test.sh bench-siqs-shadow kernel --fb "$fb" --rows "$rows" --weight 20 --workers 1,4 --parallel-threshold 0 --warmups 2 --reps 3
done

./scripts/test.sh bench-siqs-shadow prepare --fb 130000 --rows 130100 --weight 20 --warmups 2 --reps 3
./scripts/test.sh bench-siqs-shadow fbcheck --fb 130000 --inner 130100 --warmups 2 --reps 3
```

The disabled `SiqsShadowMatrixBench` CTest entry provides discovery only. Use
the runner above for measurements because it enforces the Release build and
output contract.

### Production Observe RSS Probe

The dedicated commands exercise the fixed
`siqs50_production_shadow_observe_v1` profile through the production 1LP
collector:

```bash
./scripts/test.sh probe-siqs-shadow-observe off
./scripts/test.sh probe-siqs-shadow-observe observe
./scripts/test.sh compare-siqs-shadow-observe
```

Each probe launches exactly one fresh Release/NDEBUG process and calls
`factor()` once. A successful process emits one closed
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1` record. Its `mode`,
`sample_ordinal`, canonical `factor_identity`, factor wall time, RSS fields,
`route=legacy_result`, and `promotion=false` describe that process only. An
`observe` process must also produce exactly one valid
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1` record from the production seam. An `off`
process must produce none. A timeout, nonzero exit, malformed or partial
record, unexpected diagnostic output, missing factor, or factor mismatch
invalidates the sample. The driver does not retry a failed process.
`test_siqs_shadow_proof_observe_probe` is a manual measurement executable; it
is not a CTest entry or part of a routine test tier.

The comparison builds once and launches four fresh processes: one `off` and
three `observe` samples with distinct ordinals. It emits one closed
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_COMPARISON_V1` record only after all four
records pass their individual contracts. The comparison requires canonical
factor parity, valid proof and matrix-shape evidence in every observe sample,
and internally consistent RSS support and monotonicity. It intentionally
records `identity_compared=false`; raw counts, corpus fingerprints, dependency
fingerprints, and winning dependency ordinals may vary between production
runs.

The RSS quantities have narrow, schema-specific meanings:

- In `PROBE_V1`, the before snapshot occurs immediately before `factor()` and
  the after snapshot occurs after the legacy result returns. Its peak growth is
  the additional process HWM across the complete factor call.
- In the observe V1 telemetry, `before_peak_rss_bytes` is the process HWM from
  launch through the post-join, pre-shadow snapshot.
- The observe telemetry's `after_peak_rss_bytes` is the HWM through shadow
  completion. Because raw relations remain live throughout the facade, it
  covers the production raw/shadow overlap when that overlap establishes the
  process HWM.
- Every supported `peak_growth_bytes` is exactly the matching
  `after_peak_rss_bytes - before_peak_rss_bytes`. It is additional HWM within
  that record's scope, not an allocation total. An earlier peak can censor the
  growth to zero.
- Current-RSS fields are endpoint diagnostics for their stated scope. In the
  observe record, the after snapshot occurs after shadow-owned state has been
  released. Allocator retention prevents treating it as live-object size.
- The `off` process and observe min/max values describe a small process
  distribution. Their difference is not a paired corpus delta because the
  production collector does not promise cross-process corpus identity.

This first comparison reports min/max wall time, peak RSS, and peak growth but
sets `timing_threshold_applied=false` and `rss_threshold_applied=false`. It
also fixes `prefer_scope=explicit_experiment_only`,
`shadow_outcome_routed=false`, and `promotion=false`. When every required RSS
observation is available from one consistent backend, it reports
`experiment_eligibility=candidate`; otherwise it reports
`rss_evidence=unavailable` and `experiment_eligibility=insufficient_evidence`.
Do not derive or freeze a budget from ad hoc runs. A later change must declare
the sample set and threshold before collecting promotion evidence.

### Sealed Holdout RSS Gate

All samples produced while defining or debugging the current protocol are
`calibration_excluded`. They may validate schema, backend support, and
measurement scale, but they cannot enter a later promotion decision.

The outcome-blind corpus seal is now frozen in the source-private
`src/siqs/shadow_proof_rss_holdout_fixture_internal.hpp`. Its corpus ID is
`siqs50_shadow_observe_rss_holdout_v1`. It contains eight new, balanced,
50-digit semiprimes derived from public decimal base and stride constants with
GMP `mpz_nextprime`. Canonical factor ordering and a stable, non-cryptographic
128-bit identity digest bind the ordered corpus identity.

The frozen selection contract is:

| Field | Value |
|---|---|
| Selection protocol | `gmp_nextprime_decimal_stride_v1` |
| `p` base / stride | `2100000000000000000000000` / `11000000000000000000000` |
| `q` base / stride | `8100000000000000000000000` / `17000000000000000000000` |
| Fixture mapping | For zero-based `i`, form each seed as `base + i * stride`; `mpz_nextprime(seed)` selects the factor strictly greater than that seed |
| Stable identity digest | `low=303806906129662515`, `high=18179245792498443738` |

The digest domain is `gnfs.siqs.shadow_observe_rss_holdout.v1`, and its schema
version is 1. The digest covers the corpus and selection IDs, all base and
stride constants, the sealed and calibration flags, fixture count, frozen
timeout metadata, and every ordered fixture identity field. Changing any of
those inputs creates a different corpus identity. This digest is an identity
checksum, not a cryptographic authenticity proof.

`tests/test_siqs_shadow_observe_rss_holdouts.cpp` checks only the versioned
identity, fixture count, decimal width, seed generation, canonical factor
order, probable primality, factor product, uniqueness, and digest. It does not
call production `factor()`, the observe probe, or an RSS measurement path. No
production factorization result, shadow proof record, timing sample, or memory
measurement has been collected for these inputs. The corpus is therefore
sealed but unopened. The existing one-`off` plus three-`observe` V1 evidence
remains calibration-only and cannot enter this gate.

Before opening the sealed corpus, preregister all of the following without
using holdout results to retune the implementation:

- the frozen eight-fixture corpus identity and stable digest;
- three `off` and seven `observe` fresh processes for every fixture and backend;
- separate Darwin, Linux, and Windows evaluations rather than pooling their RSS
  distributions;
- the approved deployment memory budget and its reserved headroom;
- the exact OS, architecture, RSS backend, resolved production sieve worker
  count, candidate revision, and approval identity for each platform policy;
- the deployment-owned trusted-base ID, journal-store ID, and one portable
  ASCII store locator below that base;
- the rule that compares every absolute observe-process peak with the remaining
  budget after headroom.

#### Pure Typed Gate Contract

`include/gnfs/siqs/shadow_proof_rss_gate.hpp` implements a pure typed gate over
caller-owned `SIQSShadowProofRssGatePolicy` and
`SIQSShadowProofRssGateSample` values. The allocation-free, `noexcept`
`evaluate_siqs_shadow_proof_rss_gate` function does not read environment
variables, run `factor()`, invoke the observe probe, collect process memory, or
construct a campaign. A null policy returns
`status=blocked reason=policy_missing`.

An approved policy binds `approval_id`, the sealed corpus ID and digest,
operating system, architecture, RSS backend, resolved production sieve worker
count, candidate revision, deployment memory budget, reserved headroom, and a
versioned journal-store binding. The binding contains a trusted-base ID, a
store ID, and one canonical lowercase ASCII relative locator. It deliberately
contains no absolute path, inode, volume file ID, mount point, or caller-chosen
record path.
`SIQSShadowProofRssOperatingSystem` is closed over `unknown`, `darwin`, `linux`,
and `windows`; `unknown` is invalid for an approved binding.
`SIQSShadowProofRssArchitecture` similarly admits `unknown`, `x86_64`, and
`arm64`, with `unknown` invalid for an approved binding. The selected RSS
backend must be supported and match the operating system. The budget and
headroom remain optional so an incomplete deployment envelope can produce a
typed `blocked` outcome. A present budget must be greater than headroom before
the policy is a valid gate input.

The gate requires exactly 80 records for one bound policy, operating system, and
backend. Each of the eight sealed fixture IDs must have exactly three `off` and
seven `observe` fresh-process records with the complete policy binding. Every
record must report a completed fresh process and passing factor identity. An
`observe` record must report passing proof and matrix evidence plus a nonzero
absolute peak RSS. Missing, duplicate, and extra coverage cannot pass.

`SIQSShadowProofRssSampleMode` admits `unknown`, `off`, and `observe`, with
`unknown` rejected during validation. `SIQSShadowProofRssEvidence` admits
`unknown`, `not_applicable`, `pass`, and `fail`;
`SIQSShadowProofRssFactorIdentity` admits `unknown`, `pass`, `fail`, and
`not_checked`. The gate rejects unknown enum values and requires factor identity
`pass` for every sample. Proof and matrix evidence affect structural validity
only for `observe`; their values remain diagnostic for `off`.

Absolute observe-process peak RSS is the only deciding quantity. The evaluator
computes `rss_limit_bytes = deployment_budget_bytes -
reserved_headroom_bytes` and compares the maximum validated observe peak with
that limit. `max_observe_peak_rss_bytes <= rss_limit_bytes` passes, so equality
is accepted.

The `off` peak, off/observe differences, `current_rss_bytes`,
`peak_growth_bytes`, and `wall_ns` remain diagnostics. They are absent from the
decision and cannot rescue an observe peak above the approved envelope or fail
an otherwise conforming sample.

`SIQSShadowProofRssGateStatus` is closed over `blocked`, `invalid`,
`limit_exceeded`, and `manual_review_candidate`. Policy absence, lack of
approval, and missing budget or headroom remain blocked. Invalid policy or
sample contracts return `invalid` with a specific `SIQSShadowProofRssGateReason`.
An over-limit peak returns `status=limit_exceeded
reason=observe_peak_over_limit`. A passing gate returns only
`status=manual_review_candidate reason=all_observe_peaks_within_limit`. Every
outcome retains
`shadow_outcome_routed=false` and `promotion=false`.

A terminal `SIQSShadowProofRssGateOutcome` carries a stable, non-cryptographic
`policy_binding_digest` over every policy field. Together with re-evaluation
and exact outcome matching, this identity checksum rejects ordinary relabeling
across approval, revision, corpus, operating system, backend, worker count,
journal-store binding, budget, or headroom changes. It is not a cryptographic
authenticity proof.

`emit_siqs_shadow_proof_rss_gate_outcome` accepts a `FILE*`, the policy pointer,
the complete sample span, and the supplied outcome. It re-evaluates the inputs,
requires an exact outcome match, validates the complete policy binding, and
writes one closed record with prefix `GNFS_SIQS_SHADOW_PROOF_RSS_GATE_V3` only
after those checks. The record publishes both probe SHA-256 fields plus
`policy_binding_digest_low` and `policy_binding_digest_high`. A successful
write and flush commits this audit record only; it does not authorize routing
or promotion. The emitter is
terminal-only. It accepts `limit_exceeded` and `manual_review_candidate`;
`blocked` and `invalid` remain typed outcomes but do not produce an audit line.

`tests/test_siqs_shadow_proof_rss_gate.cpp` exercises policy binding, exact
coverage, the inclusive limit, diagnostic independence, outcome mutation,
relabel rejection, and the terminal-only closed emitter. It uses only synthetic
policies and records. The project has no approved per-platform policy. No
deployment budget, reserved headroom, OS/architecture/backend binding, resolved
production sieve worker count, candidate revision, approval, or numeric
threshold is frozen. No sealed holdout has been run, and no real gate result
exists. The approved production campaign runner and measurement remain blocked
and pending. Nothing may construct or launch the 80-process campaign until the
policy is approved. The private synthetic single-slot transaction described
below is an integration proof only and cannot select the production probe or
open a holdout.

### Policy-Gated Campaign Preparation

Campaign preparation is split into pure contracts that remain usable without
opening the sealed holdouts. `include/gnfs/siqs/runtime_facts.hpp` owns the
single production sieve-worker resolution rule. `factor()` resolves that value
once and returns the actual value in `SIQSResult::resolved_sieve_workers`, so a
future probe can report the worker count used by the measured run instead of
querying the host a second time.

`include/gnfs/siqs/shadow_proof_rss_policy_record.hpp` defines the canonical
single-line `GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V3` codec. It accepts exactly one
printable-ASCII record terminated by one LF, with a fixed field order,
canonical boolean and unsigned-integer forms, and two strict lowercase
64-character SHA-256 fields. The parsed record owns its token storage and can
provide a temporary `SIQSShadowProofRssGatePolicy` view while the record
remains alive, unchanged, and unmoved. Rvalue view construction is deleted.
The codec validates representation only. Approval, frozen corpus identity,
OS/backend compatibility, nonzero worker count, trusted-base and journal-store
IDs, canonical lowercase relative-locator syntax, execution-identity validity,
and budget semantics continue to use the gate's shared policy preflight. No
approved policy record is stored in the repository.

`include/gnfs/siqs/shadow_proof_rss_campaign.hpp` consumes an already-typed
policy and performs no I/O. A missing, unapproved, incomplete, or invalid policy
returns an empty plan. A valid synthetic policy produces exactly 80 abstract
slots in frozen fixture-major order: `off` ordinals 1 through 3 followed by
`observe` ordinals 1 through 7 for each fixture 1 through 8. Every slot binds
the complete policy and its stable digest. Campaign concurrency is the constant
one and is not configurable.

`include/gnfs/siqs/shadow_proof_rss_campaign_journal.hpp` adds a pure,
write-once replay state machine around that plan. It distinguishes an absent
journal from a present header with no records. The absent state may contain
neither a header nor records and requests header creation. A valid present,
header-only state requests the first slot start. Later records must form one
digest-linked `slot_started`/`slot_committed` pair per frozen slot, in order.
`campaign_tainted` is valid only immediately after one unmatched start and only
as the final record; taint before a start, after a commit, or before trailing
records is rejected.

A proposed start record does not authorize process launch. Only the storage
layer may acknowledge the exact record after it reaches a durable commit
boundary, which yields the move-only launch permit required to construct its
matching commit. A replay ending in a start without its commit is permanently
tainted and returns only the matching taint record for durable append, never a
retry or launch action. An explicit taint record has the same terminal effect.
After all 80 commits validate, sample reconstruction replays the original
header and records again before the caller invokes the existing RSS gate. The
V3 journal also binds a closed probe classification and the two-part probe
execution identity through runtime facts, header, plan, every commit, and the
joined artifact. A complete `synthetic_test` campaign terminates as
`synthetic_complete` with no gate action; only consistently tagged
`production_holdout` data reaches the data-level reconstruction contract.

The move-only capabilities prevent accidental reuse within one replay result;
they do not prove filesystem durability or serialize two callers that replay
the same stale snapshot. The native store now constructs the durable-record
receipt only after it publishes and rereads the exact start record through its
held root. The receipt is consumed immediately to create a private launch
permit. Neither capability crosses the public store boundary. The deployment
policy binds one trusted-base ID, store ID, and canonical lowercase ASCII
relative locator, which flow through the policy, plan, header, and record
digests. Before any filesystem I/O, the native store validates the approved
policy and selects the unique matching row from a production-owned logical
registry. The row owns the absolute trusted base path, expected native owner,
store ID, locator, probe classification, complete approved policy, and expected
runtime contract. Public policy and runtime values are untrusted claims. The
store compares every field, validates the row through the same pure preflight,
and constructs the native session only from row-owned values. A malformed
executable contract, including a relative path, revision mismatch, invalid
environment, configured-owner mismatch, or invalid timeout, fails before any
journal object opens. A production row must contain a probe binding and cannot
install the private publication test seam. The expected build-mode values
remain deployment assertions, not independently observed host facts. The row
is part of the deployment, not a caller-injected callback: the public API
accepts no path, resolver, base handle, or registry installer. The POSIX loader
component-walks that owned path for each session,
obtains a held base descriptor, opens exactly the provisioned locator without
following links, and verifies the mapping's `store_id` before it scans the
root.

The store holds the root's runtime directory-object identity and an exclusive
cross-process session lease beginning before replay. Consuming
`begin_next_slot()` derives the only allowed header and record leaves, performs
exclusive immutable publication relative to the held root, refreshes the
strict layout and replay, and returns a move-only active-slot transaction. The
transaction owns both the lease and the private permit, while its public
interface intentionally exposes no launch operation. A private, non-installed
runner consumes the transaction directly. Before publishing a start from an
existing journal, the store re-establishes durability for the header and every
visible committed slot in causal order: start record, stdout, stderr, joined
artifact, then commit record. It revalidates held authority before each barrier
and requires one stable replay and artifact snapshot equal to the
pre-confirmation state before it may publish the next start. This prevents a
complete-looking prefix left by an earlier failed sync from being treated as
durable merely because it can be decoded after reopen. An API that accepts an
arbitrary output path or a caller-supplied successful I/O backend is not a
receipt issuer because either would permit duplicate launches from one stale
replay.

`include/gnfs/siqs/shadow_proof_rss_campaign_journal_codec.hpp` defines the
canonical V3 storage representation without opening a file. Headers are
exactly 160 bytes and records are exactly 320 bytes. Both use distinct
eight-byte magic, an explicit wire version and declared size, fixed
little-endian integers, dedicated enum tags, zeroed reserved bytes, and
semantic-digest verification. Header byte 20 and record byte 101 carry the
probe classification. Header offsets 80 and 112, and record offsets 240 and
272, carry the two raw 32-byte SHA-256 values. V1 and V2 wire data are rejected
rather than relabeled.
Optional values use a presence bitmap and require a zero payload when absent.
Decoding rejects short and trailing data and reports a closed error plus the
first failing byte offset. The codec never persists C++ object layout,
`size_t`, enum representation, or `std::optional` representation.

`include/gnfs/siqs/shadow_proof_rss_campaign_journal_layout.hpp` adds a pure
layout inspector without opening a directory. Its strict allowlist contains
one persistent `.session.lock` leaf, one exact 160-byte
`campaign-header.rjhd`, and a contiguous prefix of `record-%010u.rjrc` leaves
starting at sequence 1, with at most 160 leaves and exactly 320 bytes per leaf.
Unknown names, case variants, temporary artifacts, wrong entry kinds, invalid
link counts, wrong sizes, sequence gaps, codec failures, and filename-to-wire
sequence mismatches all fail closed. The inspector cannot sign a durable-record
receipt. Its decoded snapshot is ordinary, forgeable data that the native
loader must construct and consume internally while holding the root and
cross-process lease; it is never a public store or receipt-authority input.

`include/gnfs/siqs/shadow_proof_rss_campaign_artifact_layout.hpp` defines the
separate bounded artifact namespace under the fixed, preprovisioned
`.artifacts-v1` directory. Each of the 80 slots may own exactly one
`stdout`, `stderr`, and `joined` leaf with a canonical 10-digit slot number.
Stdout and joined records must be nonempty and no larger than 4096 bytes.
Stderr may be empty and is capped at 16384 bytes. The pure inspector rejects
unknown or duplicate leaves, non-regular files, non-unit link counts, and size
mismatches before it derives ordinary artifact seals. A second pure check
closes that snapshot against validated journal replay: every committed slot
requires all three exact seals, ready and complete states reject future
artifacts, and only the current tainted slot may retain any durable orphan
subset.

`include/gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp` and the native
implementation now close that read boundary. The public entry point accepts
only the validated policy and runtime facts. The default production deployment
registry is deliberately empty, so an otherwise valid request returns
`binding_not_registered`; there is no public path, descriptor, resolver, or
registry-installation input. Deployment packaging may replace only the private
owned registry table, whose rows include the expected native owner identity.

On POSIX systems, the loader walks the registered absolute base one component
at a time from `/`, opens every component and the registered store root with
`O_NOFOLLOW`. Every ancestor must be owned by root or the expected owner and
must reject group/other writes; the final base, store root, lock, header, and
records must be owned by the expected owner. The provisioned artifact root must
be an exact `0700` directory owned by the same principal and on the same
filesystem as the journal root. Artifact leaves must be exact `0600` regular
files with one link. macOS additionally rejects any extended ACL instead of
assuming that mode bits describe all write grants. The loader verifies the
process identity before it can create the persistent zero-length
`.session.lock`, then takes a non-blocking exclusive `flock` before scanning.
It keeps the base, journal root, artifact root, and lock descriptors alive in a
move-only session. Independent directory enumerations surround bounded double
reads of every known journal and artifact leaf. Identity, owner, group, mode,
link count, size, timestamps, bytes, root identity, and the held lock binding
must remain stable. Only then does the loader invoke both pure layout
inspectors, replay the journal, and close the artifacts against that replay.
Unknown leaves, link substitutions, gaps, malformed wire data, concurrent
leases, and snapshot changes fail closed with typed diagnostics.

The store tracks directory authority identity separately from full journal and
artifact namespace generation fingerprints. Every authority-bearing action
checks both generations before publication, including on filesystems where
adding a regular leaf does not change the directory link count. After an owned
publication, the store commits a new generation only after two stable snapshots
and exact replay and artifact validation succeed. APFS link-count changes are
rebased only inside that verified transition.
Windows builds compile an explicit
`platform_unavailable` implementation until a held `RootDirectory` HANDLE
implementation is available; they never fall back to caller-controlled paths.

The session exposes an authority-free replay view and one rvalue-qualified
`begin_next_slot()` transition. That transition consumes the session, publishes
an absent header when required, publishes exactly one pending start, and keeps
the lease and private permit inside the returned active-slot transaction. A
moved-from session, a terminal replay action, an existing target leaf, or any
root or lock drift returns a typed failure with no authority. The active slot
cannot start a child, commit a sample, or release a raw receipt or permit. Its
public consuming `taint()` transition durably closes the unmatched start. A
reopened dangling start exposes only the equivalent consuming
`append_pending_taint()` recovery. Before that recovery appends a taint record,
the held roots re-establish the complete committed prefix in the same causal
order, then confirm the exact unmatched start and strictly refresh the
unchanged replay. Failure or namespace drift at any predecessor leaves both the
next-start and taint actions closed. Neither destructor claims that a taint
record reached storage.

The private store integration may publish exactly one three-artifact batch
while the active slot still traps the lease and launch permit. It creates
stdout, stderr, and joined leaves in order relative to the held artifact root,
then rereads and validates each exact seal. Only a complete batch creates a
private artifact receipt inside the session core. A separate move-only
same-child receipt can be minted only by the private runner after one bounded
child succeeds, both pipes reach EOF, cleanup completes, and the strict join
accepts those exact bytes. The V3 joined draft records the deployment
classification and two-part execution identity. The store consumes the permit
and both private receipts,
requires receipt, deployment row, executable binding, runtime facts, header,
and commit classifications to agree, reconstructs the commit payload from the
owned evidence, revalidates the stable journal and artifact snapshots, and
publishes the matching commit. None of these capabilities crosses the public
store boundary.

The commit terminal state is classified before any recovery action. If a
failed publication leaves the intended commit leaf provably absent under a
stable refresh, the runner may append the explicit taint. If the exact intended
leaf is visible after a non-durable publication result, the store confirms
that immutable leaf and strictly refreshes the replay before accepting the
commit. A partial leaf, different leaf, failed confirmation, or unstable
refresh is `commit_outcome_uncertain`; an exact leaf reported as
`already_exists` is also uncertain because this transaction cannot claim its
publication. The runner drops its authority and must not append a competing
taint. If an earlier immutable artifact is already durable, diagnostics
separately retain the last durable object, record or artifact address, and byte
count. A start conflict, partial artifact batch, or allocation failure cannot
erase the durable prefix.

This boundary assumes a trusted local filesystem and treats every process with
the expected UID as the same principal. `flock` is advisory, so it cannot
isolate a malicious same-UID process that ignores the protocol. Deployment
provisioning must also reject unapproved ACL mechanisms on platforms where
they are not inspected at runtime. Every future publication, receipt, and
launch action must revalidate the held root and lock namespace bindings before
using its authority; the current session view deliberately carries no such
action authority.

`include/gnfs/util/durable_immutable_file.hpp` and its compiled implementation
provide the lower publication boundary. The path-based publisher opens and
holds the parent directory before creating a leaf exclusively. The held-parent
variant accepts only one relative leaf, never reopens or closes the borrowed
parent handle, and syncs that exact handle as the directory boundary. Both
variants complete short writes, sync file and directory metadata, close owned
handles exactly once, and never delete, truncate, rename, or repair a failed
artifact. POSIX creation uses `openat()` relative to the held parent descriptor.
macOS uses full-sync barriers for the file and parent directory. The Windows
held-parent variant fails closed until a true handle-relative create operation
is available. A held-parent confirmation operation can reopen one existing
immutable leaf without truncation and repeat the file-directory-file barrier
sequence. Injectable file operations test both state machines, but cannot
construct a journal durable-record receipt.

Artifact seals in the pure journal remain stable accidental-corruption
identities only. They do not parse or validate child stdout/stderr or prove
which process produced the bytes. Strict stdout and stderr codecs plus an
authority-free join produce an owning, typed `uncommitted` draft bound to the
approved policy, runtime facts, and one canonical slot. That draft cannot call
the private publisher, construct a journal commit payload, issue a receipt, or
grant a launch permit. The private runner now owns child launch, bounded
capture, wait status, strict join, artifact publication, and commit as one
same-child transaction. It accepts only the active slot; the executable,
candidate revision, complete environment, timeout, canonical arguments, and
capture limits come from the private deployment row and frozen slot. The
ordinary data-only transport result cannot mint either private receipt.

CMake declares `test_siqs_shadow_proof_rss_holdout_probe` as a Release-only,
single-sample production target with `EXCLUDE_FROM_ALL`. Default builds do not
build it, and neither CTest nor any runner catalog executes it. The paired
`test_siqs_shadow_proof_rss_holdout_probe_contract` target is an instant pure
contract test. `test_siqs_shadow_proof_rss_campaign_journal` is also an instant
pure contract test, as are its codec and layout tests. The strict stdout codec,
stderr codec, and authority-free stream-join tests are also instant and use
synthetic records only. `test_bounded_child_process` runs a synthetic fake
executable to cover shell-free argument/environment transfer, independently
bounded dual-pipe capture, deadlines, overflow, descendant writers, and cleanup
semantics. The transport is a production utility, but it carries data only and
is not a campaign launcher. The instant
native-store test uses a temporary real filesystem and subprocesses to cover
the registry boundary, component walking, strict layouts, move-only lease
ownership, cross-process contention, crash release, and replay actions. It also
links one `EXCLUDE_FROM_ALL`, non-installed private runner and five synthetic
child variants. Those variants read only the compiled sealed identity manifest;
they never call `factor()`, collect a holdout measurement, or invoke the
production probe. They cover successful same-child commit, nonzero exit,
malformed output, capture overflow, timeout cleanup, partial artifact
publication, explicit taint, and commit-terminal uncertainty. The success path
covers both an empty-stderr `off` slot and the first `observe` slot with one
strict typed stderr record and `pass` proof and matrix evidence. None of these
targets is a production campaign runner.

The pure probe protocol binds each `fixture_id` to the exact modulus and
canonical factor pair in the sealed constexpr manifest. Relabeling one row as
another fixture is invalid even when the decimal shape and factor fields are
otherwise self-consistent.

The production binary accepts exactly one fixture ID, one mode, and one
mode-specific ordinal. One fresh process calls `factor()` once. A successful
stdout record uses `GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1`, binds the
sealed corpus identity, reports the canonical factors and actual sieve-worker
count, and records the lifetime peak captured after the complete factor call as
`absolute_peak_rss_bytes`. The record keeps route and promotion closed. It does
not claim proof or matrix evidence. In `observe` mode, stdout success is closed
unless the production observe record was also written and flushed successfully
to stderr. For a future collector, `off` must have an empty stderr stream, while
`observe` must have exactly one independently validated production
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1` record. Only that two-stream join may derive
proof and matrix evidence for a gate sample.

The project still has no approved policy or production campaign runner.
Operators must not build or invoke the production target manually to bypass
policy preflight or open a sealed holdout. A future approved serial runner must
validate the policy, host facts, and candidate revision before it invokes this
single-sample target. The existing
`GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1` calibration path remains unchanged
and calibration-excluded; it does not provide holdout gate evidence.

The pure preparation contracts above may inspect the constexpr sealed manifest
and derive the deterministic production parameter, multiplier, factor-base,
and one-large-prime profile used to validate a slot. They do not resolve a
probe path, create an output directory, construct a production child-process
command, sample RSS, or call `factor()`. The approved-policy execution path
around the production holdout probe remains pending. The pure journal contract
still performs no file I/O.

The native leased store now combines the canonical codec and held-root durable
publisher for header and start records. It validates the resulting strict
snapshot before privately issuing the launch permit, then traps the permit
inside a lease-owning active-slot transaction. The private integration target
connects the portable transport, strict join, three-leaf artifact publication,
private same-child receipt, and commit publication without exposing a public
authority-bearing API. It also owns explicit taint publication for every
failure whose terminal commit leaf is provably absent. An approved
per-platform policy, same-object executable authentication, and the serial
80-slot campaign loop remain pending. Those authority-bearing components must
validate the approved policy, actual runtime facts, and execution identity
before loading the sealed fixture table or constructing the first production
command. Synthetic committed prefixes validate the transaction machinery only.
V3 now binds their `synthetic_test` classification and two-part execution
identity durably, and it makes even a complete 80-slot synthetic journal
gate-ineligible. The private deployment row owns the approved executable
SHA-256 and canonical execution-contract SHA-256. The contract covers the
platform, build mode, exact sorted environment, timeout, owner, argument
template, stream schemas, capture limits, and transport guarantees. The store
recomputes it before filesystem access, and the runner carries it through the
joined draft, same-child receipt, artifacts, and commit.

This is still a deployment-claim boundary, not executable-image
authentication. The current `lstat(path)` followed by path-based spawn does
not bind the executed file object to the approved executable digest. Linux
still needs sealed-descriptor hashing plus descriptor-based execution. macOS
still needs signed-code validation of a suspended child before resume, or it
must remain unavailable. Authority-held gate evaluation and the serial
80-slot controller also remain pending. A campaign interrupted after its
durable start but before its sample commits remains tainted and cannot be
retried in place.

`tests/test_siqs_runtime_facts.cpp`,
`tests/test_siqs_shadow_proof_rss_policy_record.cpp`,
`tests/test_siqs_shadow_proof_rss_campaign.cpp`,
`tests/test_siqs_shadow_proof_rss_campaign_journal.cpp`,
`tests/test_siqs_shadow_proof_rss_campaign_journal_codec.cpp`,
`tests/test_siqs_shadow_proof_rss_campaign_journal_layout.cpp`,
`tests/test_siqs_shadow_proof_rss_campaign_artifact_layout.cpp`, and
`tests/test_siqs_shadow_proof_rss_holdout_probe_contract.cpp` cover only
injected values, synthetic metrics, and constexpr fixture identity. They do not
run a probe, factor a holdout, or read live process memory.

`tests/test_siqs_shadow_proof_observe_record_codec.cpp`,
`tests/test_siqs_shadow_proof_rss_holdout_probe_record_codec.cpp`, and
`tests/test_siqs_shadow_proof_rss_holdout_stream_join.cpp` cover strict wire
decoding, cross-stream invariants, and uncommitted draft ownership with
synthetic records. The join test independently derives each fixture's current
production factor-base profile, but none of these tests launches a child,
calls `factor()`, samples RSS, publishes an artifact, or opens the holdout.

`tests/test_bounded_child_process.cpp` launches only its synthetic fake child.
It has no sealed-fixture, policy, journal, receipt, or launch-permit interface.

`tests/test_siqs_shadow_proof_rss_campaign_journal_store.cpp` opens only
temporary synthetic stores. It exercises native object identity, leases,
held-root header/start and artifact publication, strict reread, crash recovery,
explicit taint, injected publication failures, private same-child commits, a
complete 80-slot synthetic terminal state, and restart relabel rejection. Its
children emit synthetic protocol records only; the test does not open a sealed
holdout, run the production probe, or collect campaign evidence.

`tests/test_durable_immutable_file.cpp` uses temporary synthetic bytes to cover
exclusive-create contention, partial writes, interrupted calls, zero progress,
file and parent sync failures, close failures, existing leaves, and symlink
leaves. It also verifies that held-parent publication neither reopens nor closes
the borrowed directory handle. It never opens a holdout or issues a launch
permit.

### Pure V2 Prefer Boundary

The V2 prefer layer is a pure decision and audit boundary. The environment
parser still rejects `prefer`; neither `factor()` nor the production observe
seam calls the V2 contract, and no shadow result is routed. The pure header
`include/gnfs/siqs/shadow_proof_prefer.hpp` exposes
`evaluate_siqs_shadow_proof_prefer`, `finalize_siqs_shadow_proof_prefer`, and
`emit_siqs_shadow_proof_prefer_decision`. Its closed record prefix is
`GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2`.

The record's `next_route` is a recommendation created at
`emit_phase=before_route`. It does not report a completed route. A future
adapter may commit `next_route=shadow_return` only after the complete record is
written and flushed successfully with no stream error, so the emitter returns
`true`. Construction, partial-write, write, flush, or stream-error failure
continues the unchanged legacy path. This remains truthful even when a failed
stream exposes bytes from the pre-route record because the record never claims
that the route was applied.

A future shadow `SIQSResult` uses the selected shadow matrix row count for
`relations_found`, the post-join production sieve counter for
`polynomials_used`, and the same pre-emit decision wall-time sample for
`time_seconds`, measured from the existing SIQS timer start. The caller samples
after pure proof/factor/evidence evaluation and accepted-factor copying, but
before `finalize_siqs_shadow_proof_prefer` and emitter I/O. A future
`SIQSResult` copies that value without resampling.

## Dense Solver Results

At revision `0baa518`, parallel elimination created `std::jthread` workers for
every pivot. The benchmark set the parallel threshold to zero to measure that
legacy path. This historical full-solver baseline motivated retaining workers:

| Shape | Worker mode | Median | Range | Relative to serial |
|---|---|---:|---:|---:|
| 50-digit synthetic, 1600 equations × 1700 rows | serial | 20.175ms | 19.991–20.754ms | 1.00× |
| 50-digit synthetic | `jthread` × 2 | 42.972ms | 40.216–45.071ms | 2.13× slower |
| 50-digit synthetic | `jthread` × 4 | 62.446ms | 61.867–66.049ms | 3.10× slower |
| 70-digit synthetic, 15000 equations × 15100 rows | serial | 9.102s | 8.909–9.523s | 1.00× |
| 70-digit synthetic | `jthread` × 2 | 5.483s | 5.425–5.642s | 1.66× faster |
| 70-digit synthetic | `jthread` × 4 | 4.506s | 4.352–4.669s | 2.02× faster |

Every one-, two-, and four-worker full-solver run produced the same dependency
digest.

The current kernel benchmark compares three exact implementations:

- `legacy_per_pivot_jthread` recreates workers at every pivot;
- `benchmark_only_queued_thread_pool` retains threads but still allocates one
  task/future batch per pivot;
- `production_persistent_worker_team` submits one long-lived task per worker,
  then publishes each pivot through a mutex-protected generation and completion
  count without per-pivot task allocation.

All paths keep the same fixed contiguous row partitions. Where parallel work is
selected, pool or team construction and one empty prewarm or no-op startup
dispatch are outside the timed scope. Revision `a5c127a` produced:

| Shape | Workers | Legacy `jthread` | Queued prototype | Production team | Production vs legacy |
|---|---:|---:|---:|---:|---:|
| 50-digit synthetic | 1 | 15.688ms | 15.637ms | 15.565ms | serial control |
| 50-digit synthetic | 2 | 39.180ms | 18.836ms | 19.800ms | 49.5% faster |
| 50-digit synthetic | 4 | 52.090ms | 22.879ms | 22.050ms | 57.7% faster |
| 70-digit synthetic | 2 | 5.436s | 4.900s | 5.062s | 6.9% faster |
| 70-digit synthetic | 4 | 4.161s | 3.751s | 3.783s | 9.1% faster |

Every row in this table has the same final matrix digest across all three
implementations and requested worker counts. The production team is created
lazily at the first nonzero parallel pivot. Partial submission and worker
exceptions return `worker_failure`; the test suite verifies recovery with a
fresh team. Debug, Release, AddressSanitizer plus UndefinedBehaviorSanitizer,
and TSan runs all pass.

Four production workers remain slower than the worker-one control at the
50-digit shape. The default threshold therefore remains 20000 equations, which
keeps the 50- and 70-digit synthetic shapes serial until live row distributions
justify a lower value. Earlier queued-prototype evidence showed a four-worker
gain beginning near 4000 factor-base columns:

| Factor-base columns | Serial | Queued pool × 4 | Improvement |
|---:|---:|---:|---:|
| 2500 | 47.421ms | 45.899ms | 3.2% |
| 4000 | 159.828ms | 97.693ms | 38.9% |
| 5500 | 485.484ms | 209.015ms | 57.0% |
| 8000 | 1.300s | 496.945ms | 61.8% |
| 10000 | 2.532s | 809.467ms | 68.0% |

The crossover table's serial column is `implementation=legacy_per_pivot_jthread`
with `workers=1`. The benchmark-only pool is not constructed for one worker.

These crossover values are provisional. A production threshold must use a
frozen live corpus and account for `affected_rows * words_per_row`, not only a
static equation count.

## Shared Validation Cost

The solver previously validated the full factor base once at entry and again
inside every row identity check. On a synthetic 90-digit-shaped corpus with
130000 total factor-base entries and 130100 rows, the repeated scan dominated
preparation. The live span would add one sign-sentinel column and one target
row.

| Validation path | Median | Range |
|---|---:|---:|
| Public per-row wrapper | 4265.776ms | 4263.082–4282.251ms |
| Prevalidated per-row helper | 251.017ms | 245.579–252.786ms |
| Factor-base scans alone | 4047.424ms | 3976.202–4091.568ms |

Reusing the validated context is 16.994× faster and reduces this phase by
94.116%. The helper still checks source IDs, row shape, and the complete signed
arithmetic identity for every row.

## 90-Digit Dense Boundary

The packed transpose uses:

```text
bytes = factor_base_columns * ceil(shadow_rows / 64) * sizeof(uint64_t)
```

| Nominal band | Equations including sentinel | Shadow rows | Packed bytes |
|---|---:|---:|---:|
| 50-digit | 1601 | 1701 | 345816 |
| 70-digit | 15001 | 15101 | 28321888 |
| 84-digit | 30001 | 30101 | 113043768 |
| 89-digit | 80001 | 80101 | 801290016 |
| 90-digit | 130001 | 130101 | 2114336264 |

The 90-digit value is 1.969GiB for the packed matrix payload alone. It excludes
input rows, GMP values, pivot metadata, dependencies, allocator overhead, and
temporary working storage.

No full 90-digit dense solve was run. The simple cubic extrapolation is
`9.102s × (130000 / 15000)^3 ≈ 98.8min`. That number is not a benchmark and
does not include worse cache behavior.

The dense shadow solver therefore uses two independent default gates:

- at most 100000 shadow-row variables;
- at most 256MiB of packed matrix payload.

An oversized variable dimension requires a different backend and returns
`unsupported_backend`. A representable dense shape above the byte budget
returns `resource_limit`. Checked arithmetic failures remain `size_overflow`.
The five-row 90-digit constructed oracle remains below both limits because its
packed payload is about 1MiB.

## Sparse Backend Boundary

The legacy SIQS `BlockLanczos` dispatcher is not a safe shortcut for the wide
shadow path. Its internal policy selects Gaussian elimination when the
augmented matrix estimate is below 4GiB. The nominal 90-digit augmented shape
is about 3.94GiB, so that route can still select a multi-gigabyte dense solve.

A future sparse implementation must call a typed Block Wiedemann boundary
directly and preserve the matrix direction `shadow rows × factor-base columns`.
Before promotion it must provide:

- checked nonzero and compressed sparse row (CSR) storage estimates;
- typed distinction between no dependency and solver failure;
- deterministic seed and stream policy, or an explicitly weaker result
  contract;
- independent GF(2) null-vector verification against the original rows;
- sorted, deduplicated dependency ordinals before proof-gated extraction;
- a direct CSR or memory-mapped builder that avoids two simultaneous sparse
  copies at 90-digit scale.

## Promotion Gates

- [x] Exact wide row, dependency, congruence, and factor extraction contracts.
- [x] Stable 50-, 70-, and 90-digit constructed corpus across one, two, and four
  workers.
- [x] Shared-context validation with measured 90-digit-shaped improvement.
- [x] Checked dense resource and unsupported-backend boundary.
- [x] Persistent worker lifecycle, deterministic output, typed failure, and
  sanitizer-clean synchronization contracts.
- [ ] Parallel threshold frozen from live row distributions.
- [ ] Direct sparse backend with typed failure and verified dependencies.
- [x] Transactional per-polynomial relation/payload capture limit before dense
  relation allocation, with typed stop reasons and default-path parity tests.
- [x] Fixed-plan live-sieve capture across the 50-, 70-, and 90-digit bands,
  with a stable A planner, per-slot state digests, and actual one-, two-, and
  four-worker fresh-process identity checks. The first corpus has zero graph
  cycles. A separate fixed 64-A, 2048-slot 50-digit profile now produces 57
  valid cycle rows, including five cycles with accepted 2LP edges, but only 397
  selected rows. It therefore provides live cycle-density evidence without
  freezing the parallel threshold or satisfying the production row-excess
  gate.
- [x] Bounded 256-A, 50-digit assembly with 1701 selected rows and a frozen
  proof-gated factor result across one, two, and four workers.
- [x] Read-only typed proof facade with relation, payload, graph, row, matrix,
  dependency, and factor boundaries.
- [x] Strict opt-in observe seam in `factor()` with default workers set to 1,
  raw-input immutability, typed setup failures, failure-transparent telemetry,
  and the default 1LP legacy path unchanged.
- [x] Outcome-blind seal for eight new, balanced, 50-digit RSS holdouts, with a
  public deterministic generator, stable non-cryptographic 128-bit identity
  digest, and mathematical identity test that never calls production `factor()`
  or probe.
- [x] Pure typed RSS gate with closed policy binding, strict 80-record coverage,
  inclusive absolute observe-peak budget comparison, diagnostic-only secondary
  metrics, manual-review-only pass, and route/promotion disabled for every
  outcome.
- [x] Canonical fixed-width campaign-journal codec plus a fail-closed durable
  immutable-file primitive with exclusive publication and injected fault tests.
- [x] POSIX native leased journal loader with a private deployment binding,
  strict owner/permission/ACL checks, held root and lock descriptors, stable
  double snapshots, and authority-free replay views. Windows remains an
  explicit fail-closed platform stub.
- [x] Strict owning decoders for the probe stdout and observe stderr records,
  plus an approved-policy and runtime-bound join that yields only an
  authority-free uncommitted evidence draft.
- [x] Production data-only shell-free child transport with independent
  stdout/stderr caps, a monotonic deadline, process-tree cleanup, exact
  environment transfer, and POSIX and Windows implementations sharing one
  synthetic contract suite.
- [x] Durable header/start publication and private receipt issuance from the
  held root, with root and lock revalidation at every authority-bearing action
  and a lease-bound active-slot transaction that exposes no raw permit.
- [x] Fixed held artifact root with strict bounded layout and journal closure,
  three-leaf durable batch publication, private batch receipts, explicit
  durable taint, reopen recovery with full committed-prefix and dangling-start
  confirmation, and injected publication and confirmation-failure tests.
- [x] Private same-child single-slot transaction with deployment-owned
  executable, environment and timeout; bounded dual-stream capture; strict
  join; exact artifact publication; private commit authority; and fail-closed
  terminal-leaf classification. Its synthetic tests cover canonical `off` and
  first-`observe` commits without factoring or measuring the sealed production
  holdout.
- [x] V2 durable probe-classification binding across runtime facts, private
  deployment and executable rows, journal header and commits, same-child
  receipts, and joined artifacts. Complete synthetic campaigns terminate
  without gate authority; executable-image authentication remains pending.
- [x] Deployment-owned approval and expected-runtime binding. Caller policy
  and runtime values must match every private row field, and malformed runner
  configuration fails before journal I/O. These expected values are not
  executable authentication or independently observed host facts.
- [ ] Approved per-platform RSS policy. It must bind the budget, reserved
  headroom, OS, architecture, RSS backend, resolved production sieve workers,
  candidate revision, approval identity, sealed corpus digest, trusted journal
  base ID, journal store ID, and canonical lowercase relative locator.
- [ ] Fresh-process campaign runner that refuses to construct or launch the
  80-process plan until the matching per-platform policy is approved.
- [ ] Sealed holdout measurement with overlapping raw/shadow RSS evidence. Each
  platform still requires three `off` plus seven `observe` runs for each of the
  eight fixtures. The existing one-`off` plus three-`observe` V1 samples remain
  calibration-excluded, and no holdout result currently exists.
- [x] Pure V2 `prefer` decision and audit contract with fail-closed factor and
  `SIQSResult` metadata validation.
- [ ] Parser and production routing for explicit V2-audited `prefer`; a future
  emitter success is the commit point, and default promotion remains disabled.
- [ ] Controlled 2LP collector; `lp_bound_sq` remains 0.
