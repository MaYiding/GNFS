# SIQS Live-Sieve Capture Contract

## Status and Scope

This document defines the contract for bounded live-sieve evidence for the
Self-Initializing Quadratic Sieve (SIQS) two-large-prime (2LP) shadow path. A
Release-only version 1 probe now implements the fixed-plan, in-memory portion
of the contract and reports one validated `GNFS_SIQS_LIVE_CAPTURE_V1` record.
It does not authorize production 2LP collection. Persistent JSON and raw-corpus
artifacts remain a later slice.

The first implementation must answer three questions:

1. Which full, one-large-prime (1LP), and candidate 2LP relations occur under
   the live SIQS parameter table?
2. Do the strict adapter, graph, materializer, and shadow assembly preserve
   their conservation laws on those relations?
3. Does one fixed logical polynomial corpus produce identical canonical
   artifacts with one, two, and four workers?

The capture must not wrap `gnfs::siqs::factor()`. That entry point owns dynamic
relation stopping, legacy merging, linear algebra, extraction, and factor
success. Wrapping it would mix collection evidence with production policy and
factor success. The capture runner must reconstruct only the deterministic
front half: parameter selection, multiplier selection, factor-base creation,
logical polynomial planning, sieving, and isolated shadow processing.

Production `factor()` must continue to set `lp_bound_sq = 0`. The capture path
must use a separate, explicitly configured observation sink. The production
null-sink path must retain its existing control flow and must not allocate,
reserve, append, or update capture state. When configured, the probe may copy
candidate 2LP relations into an independent bounded vector. It must not change
the production relation vector, full and 1LP admission, production counters, or
production stopping behavior.

`SIQSShadowTwoLargePrimeCaptureSink` now supplies that supplemental storage
boundary. Each caller-owned sink has an independent vector, a cofactor bound,
and checked relation and logical-payload limits. One sieve worker must have
exclusive access to one sink. A valid constructor reserves
`max_relations` entries before sieving; a cofactor bound below four or either
zero cap produces a stopped sink without reserving. An impossible reservation
fails construction instead of deferring an allocation failure into capture.
The public snapshot contains only the stop reason, observed 2LP candidates,
captured relations, and captured logical payload. It does not expose unrelated
threshold or residual counters from the internal admission controller.

`sieve_polynomial()` applies the legacy residual classification first and
consults the supplemental bound only after legacy rejection. The trusted
classifier establishes that the raw cofactor is composite. The sink checks the
bounded unresolved sentinel but does not repeat primality testing or factor
splitting. A legacy full, 1LP, or enabled 2LP admission therefore wins and is
never duplicated. Reaching a supplemental cap stops only that sink; later
legacy relations and later sieve candidates continue normally.

The sink reserves logical payload before invoking its relation factory. It
recomputes the returned relation's value bytes and vector sizes, requires an
exact match with the reservation, validates the unresolved sentinel, then
appends and commits as one transaction. A factory exception or contract
violation cancels the reservation, preserves the prior vector, and propagates
to the caller. Production `factor()` passes null for both optional capture
controllers, so this slice does not enable production 2LP collection. A later
slice must construct one sink per worker and compose the retained vectors in a
deterministic logical-slot order.

## Fixed Logical B Plan

Each band uses one fixed input, one fixed seed, and one immutable logical B
plan. One logical B slot is one sieve polynomial. Its identifier is the pair:

```text
(a_ordinal, gray_ordinal)
```

The runner must build the complete plan serially before starting workers. Plan
construction performs these steps in order:

1. Select parameters from the decimal digit count.
2. Select the Knuth-Schroeppel multiplier and construct `kN`.
3. Build the factor base for `kN`.
4. Generate each A definition from the fixed seed in ascending `a_ordinal`.
   The implemented probe uses an exact GMP integer root to locate the candidate
   window and an explicit Fisher-Yates permutation driven by raw `mt19937`
   words and rejection sampling. It does not depend on the implementation-
   defined mapping used by `std::shuffle`.
5. Enumerate the requested Gray-code B ordinals in ascending order.
6. Hash the input, parameters, factor base, A definitions, B identifiers, and
   capture limits into `plan_fingerprint`.

Workers receive fixed contiguous ranges of logical B identifiers. They must not
advance a shared random-number generator, claim work through a race-dependent
queue, or stop after a shared first-arrival relation count. Each logical B slot
owns its output and applies the same local limits. The runner joins all slots in
logical B order before canonicalization.

The implemented V2 cycle-density, V3 scale, and V4 proof-shadow profiles route
this stage through `execute_fixed_slots`. Each worker owns its sieve buffers and
reconstructs the polynomial at the beginning of its static partition and at
every crossed A boundary. It advances Gray ordinals sequentially only within
one A family. An operation or Gray-transition failure cancels remaining slots,
joins every worker, and discards all internal results.
The caller validates the complete returned identity set before publishing any
slot, and `SlotCapture` has compile-time nothrow move requirements for the final
publication pass. In-process fault checks inject failures at both boundaries
and require the caller's sentinel slots to remain unchanged. This is a profile
runner property; it does not enable the production SIQS collector.

Runs with one, two, and four workers must therefore consume the same planned
corpus. A worker-count comparison is invalid unless all three records have the
same plan digest, planned B count, completed B set, per-slot state digest, raw
corpus digests, and canonical assembly fingerprints. Every requested worker
count must also be physically resolved and reach the launch gate; a four-worker
request that runs fewer than four workers is not four-worker evidence.

Each worker-count run must execute in a fresh process. The comparison driver
must not change worker counts inside one process because allocator state,
thread-pool state, and cached buffers would then cross measurement boundaries.

## Size Bands

The initial matrix covers the live 50-, 70-, and 90-digit parameter bands.
Inputs must be fixed semiprimes and recorded as decimal strings in every
artifact. The implementation must introduce one dedicated fixed semiprime
fixture for each band. Each fixture must freeze the decimal input, its known
prime factors, and an input fingerprint. Constructed shadow-corpus fixtures are
not live-sieve inputs and must not be reused as if they were.

| Band | Primary FB Size | FB Columns With Sign | Fixed B Slots | Worker Counts | Required Outcome |
|---:|---:|---:|---:|:---:|---|
| 50 digits | 1600 | 1601 | 8 | 1, 2, 4 | Complete bounded capture and conservation audit |
| 70 digits | 15000 | 15001 | 4 | 1, 2, 4 | Complete bounded capture and conservation audit |
| 90 digits | 130000 | 130001 | 4 | 1, 2, 4 | Complete bounded capture and matrix-admission projection |

The runner must assert the selected band and factor-base column count. A table
change must fail closed instead of silently changing the corpus.

## Hard Capture Limits

Wall time and resident set size (RSS) are informational fields. They must not
decide which logical B slots or relations enter the artifact. Logical work and
payload limits provide the reproducible bound.

Every logical B slot has both of these hard limits:

- `max_relations_per_b`, which bounds emitted full, 1LP, and candidate 2LP
  records before append;
- `max_logical_payload_bytes_per_b`, which bounds the sum of deterministic
  relation payload sizes before allocation and append.

The logical payload size of one relation is:

```text
ceil(bit_length(abs(value)) / 8)
+ exponents.size() * sizeof(uint8_t)
+ fb_indices.size() * sizeof(uint32_t)
+ merge_lps.size() * sizeof(uint64_t)
```

This quantity excludes allocator metadata and object control blocks. The
relation-count limit bounds those fixed per-object costs. The artifact must
record both limits and both observed totals.

The sieve must check the next relation against both limits before it allocates
or appends the relation. Reaching either limit ends only that logical B slot,
clears its reusable exponent state, and returns a typed truncation reason. The
captured count and payload must never exceed their configured limits.

Each logical B slot uses this transaction order:

1. **Reserve:** reserve the preflight-checked bounded vector capacity before
   processing the slot.
2. **Materialize:** construct the accepted capture relation in a local
   temporary after its checked logical payload fits the remaining budget.
3. **Append:** move the complete temporary into the slot-owned vector.
4. **Commit:** update committed relation and payload counters only after the
   append succeeds.

An exception during reserve, materialization, or append cancels all workers.
The runner must discard every partial slot and fail the complete artifact. It
must not publish a valid or truncated artifact assembled from the surviving
workers.

The run also has fixed `max_a_values`, `max_b_values`, and
`max_split_attempts_per_b` limits. The implementation must compute the maximum
run relation count and logical payload with checked arithmetic before launching
workers. It must also compute the candidate cofactor bound with checked
multiplication. Overflow is a configuration failure, not a truncated capture.

Expected budget saturation remains a valid observation. Configuration,
arithmetic, worker, and invariant failures invalidate the artifact. The typed
API should separate these cases:

```cpp
enum class SIQSLiveCaptureStatus : uint8_t {
    valid,
    invalid_target,
    invalid_options,
    size_overflow,
    allocation_failure,
    worker_failure,
    shadow_failure,
    internal_invariant_failure,
};

enum class SIQSLiveCaptureStopReason : uint8_t {
    complete,
    relation_limit,
    logical_payload_limit,
    split_attempt_limit,
};
```

A valid result carries every logical B slot, including its stop reason. A
failure result carries no publishable corpus.

## Candidate and Shadow Boundaries

The capture sink observes an exact unsigned 64-bit residual after factor-base
trial division. It must not change the production relation vector. Candidate
2LP records use the raw sentinel encoding expected by the strict adapter:

```text
large_prime  = unresolved composite cofactor
large_prime2 = 1
```

The capture runner must not call the legacy `merge_partials()` path. The
implemented version 1 probe passes the bounded raw corpus through adapter,
graph, and assembly, then projects the existing matrix admission gates without
running the solver. A later persistent-artifact runner must cover the complete
sequence:

```text
prepare_two_large_prime_corpus
build_two_large_prime_cycle_basis
materialize_two_large_prime_cycle_checked
assemble_siqs_shadow_rows_bounded
solve_siqs_shadow_matrix (only when its existing resource gates admit the shape)
verify_siqs_post_merge_dependency
extract_siqs_post_merge_factor
```

`normalize_two_large_prime()` remains the only admission boundary for an
unresolved candidate split. A failed split, a non-prime endpoint, an endpoint
above the large-prime bound, or an inexact product contributes to adapter
rejection statistics and never enters the graph.

## Implemented Evidence-Line Schema

Each successful process writes exactly one whitespace-separated record to
standard output and writes nothing to standard error:

```text
GNFS_SIQS_LIVE_CAPTURE_V1 schema_version=1 status=valid ...
```

The runner rejects missing, empty, or duplicate keys. It requires the Release
and `NDEBUG` build contract, frozen plan and per-slot state digests, typed
adapter rejection counters, graph and assembly conservation markers, three
assembly fingerprints, logical and canonical raw-corpus digests, and an
explicit `matrix_status_scope=projected_not_run solver_attempted=false` pair.
The plan fixture freezes the selected A and the complete plan digest before any
capture worker starts.

The one-, two-, and four-worker comparison sorts all key/value fields and
requires byte-for-byte identity after removing only `workers`,
`resolved_workers`, `peak_workers`, `wall_ns`, and `peak_rss_bytes`. The worker
fields are validated independently and must all equal the requested count.

## Persistent Artifact Schema Target

The next artifact slice will write one versioned JSON summary. An optional
bounded raw corpus file may accompany it. The target summary schema is:

```text
schema_version: uint32, exactly 1
status: one of valid, invalid_target, invalid_options, size_overflow,
        allocation_failure, worker_failure, shadow_failure,
        internal_invariant_failure
failure_stage: null when valid; otherwise one of plan, sieve, adapter, graph,
               assembly, matrix, artifact
stop_reasons:
  complete: uint64
  relation_limit: uint64
  logical_payload_limit: uint64
  split_attempt_limit: uint64

input:
  band_digits: uint32
  n_decimal: string
  multiplier: uint32
  kn_decimal: string

plan:
  seed: uint64
  workers: uint32
  a_values: uint64
  b_values_planned: uint64
  b_values_completed: uint64
  plan_fingerprint: {low: uint64, high: uint64}
  completed_b_fingerprint: {low: uint64, high: uint64}
  truncated_b_fingerprint: {low: uint64, high: uint64}
  b_slots: array of:
    a_ordinal: uint64
    gray_ordinal: uint64
    stop_reason: one of complete, relation_limit, logical_payload_limit,
                 split_attempt_limit
    relations: uint64
    logical_payload_bytes: uint64
    split_attempts: uint64

parameters:
  fb_size: uint32
  factor_base_columns: uint64
  sieve_half: uint32
  lp_multiplier: uint32
  lp_bound: uint64
  capture_cofactor_bound: uint64
  max_a_values: uint64
  max_b_values: uint64
  max_relations_per_b: uint64
  max_logical_payload_bytes_per_b: uint64
  max_split_attempts_per_b: uint64

capture:
  raw_full: uint64
  raw_one_lp: uint64
  raw_two_lp_candidates: uint64
  raw_total: uint64
  logical_payload_bytes: uint64
  split_attempts: uint64

adapter:
  input_relations: uint64
  full_relations: uint64
  accepted_one_lp: uint64
  accepted_two_lp: uint64
  rejected_relations: uint64
  malformed_source_shape: uint64
  unsupported_encoding: uint64
  invalid_one_large_prime: uint64
  invalid_two_large_prime_split: uint64
  exact_duplicate: uint64

graph:
  vertices: uint64
  edges: uint64
  components: uint64
  cycles: uint64

assembly:
  encoded_full_relations: uint64
  valid_full_relations: uint64
  rejected_full_relations: uint64
  full_sources: uint64
  duplicate_full_sources: uint64
  partial_sources: uint64
  valid_cycle_rows: uint64
  rejected_cycle_rows: uint64
  rows_before_dedup: uint64
  arithmetic_duplicates_removed: uint64
  pretrim_rows: uint64
  selected_rows: uint64
  selected_full_rows: uint64
  selected_cycle_rows: uint64
  trimmed_rows: uint64
  source_catalog_fingerprint: {low: uint64, high: uint64}
  pretrim_rows_fingerprint: {low: uint64, high: uint64}
  selected_rows_fingerprint: {low: uint64, high: uint64}

matrix:
  status: one of not_run, valid, invalid_modulus, invalid_factor_base,
          invalid_options, size_overflow, invalid_row, row_identity_mismatch,
          worker_failure, internal_invariant_failure, resource_limit,
          unsupported_backend
  rows: uint64
  columns: uint64
  dependencies: uint64 or null

artifact:
  raw_corpus_present: bool
  raw_corpus_file: relative string or null
  raw_corpus_bytes: uint64 or null
  raw_corpus_fingerprint: {low: uint64, high: uint64} or null

informational:
  wall_nanoseconds: uint64
  peak_rss_bytes: uint64 or null
  build_type: string
  compiler_id: string
  compiler_version: string
  git_revision: string
```

The raw corpus artifact, when requested, must include `schema_version`,
`plan_fingerprint`, each logical B identifier, candidate position, relation
encoding, and a whole-file fingerprint. The JSON summary records that
fingerprint. Artifact consumers must reject a schema mismatch or fingerprint
mismatch.

## Required Conservation Checks

The runner must evaluate every equation with checked unsigned arithmetic. A
false equation invalidates the artifact.

```text
capture.raw_total
  = capture.raw_full
  + capture.raw_one_lp
  + capture.raw_two_lp_candidates

adapter.input_relations = capture.raw_total

adapter.input_relations
  = adapter.full_relations
  + adapter.accepted_one_lp
  + adapter.accepted_two_lp
  + adapter.rejected_relations

adapter.rejected_relations
  = adapter.malformed_source_shape
  + adapter.unsupported_encoding
  + adapter.invalid_one_large_prime
  + adapter.invalid_two_large_prime_split
  + adapter.exact_duplicate

assembly.partial_sources
  = adapter.accepted_one_lp + adapter.accepted_two_lp
  = graph.edges

graph.cycles = graph.edges - graph.vertices + graph.components

assembly.encoded_full_relations
  = assembly.valid_full_relations + assembly.rejected_full_relations

assembly.valid_full_relations
  = assembly.full_sources + assembly.duplicate_full_sources

assembly.rows_before_dedup
  = assembly.full_sources + assembly.valid_cycle_rows
  = assembly.pretrim_rows + assembly.arithmetic_duplicates_removed

assembly.pretrim_rows
  = assembly.selected_rows + assembly.trimmed_rows

assembly.selected_rows
  = assembly.selected_full_rows + assembly.selected_cycle_rows
```

The graph equation uses the represented vertex and connected-component counts,
including virtual vertex `0` when at least one 1LP edge uses it. The empty graph
uses zero vertices, zero edges, zero components, and zero cycles.

Across one, two, and four workers, all non-informational configuration, count,
stop-reason, graph, assembly, and fingerprint fields must match. Only the
requested, resolved, and observed worker counts plus wall time and peak RSS may
differ; all five are separately validated.

Cycle materialization has a typed, fail-closed boundary. Size or exponent
overflow terminates assembly as `size_overflow`. Invalid source catalogs,
cycle support, source shape, odd large-prime degree, or another materializer
invariant failure after the strict adapter and graph terminates assembly as
`internal_invariant_failure`. Workers write only their fixed cycle slots, and
the joined outcomes are reduced in cycle order. Only a later exact
row-identity mismatch contributes `rejected_cycle_rows`.

Bounded assembly carries the proof envelope through its owning rebuild. Graph
edge, cycle, and incidence caps are checked by the graph builder before cycle
slot allocation; the candidate-row cap is checked before cycle slots and the
combined row vector; and the pre-trim cap is checked before the selection mask.
The compatibility `assemble_siqs_shadow_rows` overload remains unlimited for
existing direct callers, while the proof facade always calls
`assemble_siqs_shadow_rows_bounded` with its frozen limits.
If the owning rebuild alone reaches a graph or candidate-row limit after the
same preflight passed, the proof facade treats that difference as a splitter
purity or corpus invariant failure. The pre-trim limit remains a normal
bounded fallback because its exact count is observed only by assembly. Both
row-limit statuses retain their observed and maximum scalar values for
in-memory diagnostics while discarding every partial assembly object. This
change does not extend the stable V1 observe-record schema, which continues to
carry the terminal status but not the two scalar values.

## Zero-Cycle and Promotion Rules

A zero-cycle artifact is valid when every conservation check passes. It is
evidence about the bounded live distribution, not evidence that the graph or
materializer failed.

Zero cycles do not satisfy the 2LP promotion gate. Production promotion still
requires useful live cycles, valid materialized identities, stable assembly and
dependency fingerprints, and successful proof-gated extraction on an admitted
matrix backend. A truncated logical B slot also prevents promotion, even when
its partial artifact remains valid for capacity planning.

## The 90-Digit Sparse-Backend Boundary

The 90-digit band has 130001 factor-base columns. The capture runner may build
and audit its bounded adapter, graph, and assembly artifacts. It must not
override the existing shadow solver limits of 100000 variables and 256MiB of
packed dense matrix payload.

If the bounded row shape exceeds either gate, the matrix stage must retain the
typed `unsupported_backend` or `resource_limit` result. The runner must not
allocate a larger dense matrix, substitute the legacy matrix path, or count the
capture as sparse-backend evidence. Production 90-digit promotion requires a
direct sparse backend with typed failure and verified dependencies.

The current fixed 90-digit capture produces one full row. Its projected dense
shape is admitted, so it does not exercise either sparse-backend gate. This is
valid live-distribution evidence, not evidence that the 90-digit sparse boundary
has been implemented or tested.

## Implemented Runner Interface

The single-process command performs one Release-only capture. The comparison
command builds once and launches three fresh probe processes:

```bash
./scripts/test.sh probe-siqs-live-sieve 50 1
./scripts/test.sh compare-siqs-live-sieve 50
./scripts/test.sh compare-siqs-live-sieve 70
./scripts/test.sh compare-siqs-live-sieve 90
```

Both commands reject `--no-build`, explicit `--retry`, and non-Release build
types. The probe executable also checks its configured build type and `NDEBUG`
before reading the fixture.

## Implemented Multi-A Cycle-Density Profile

The fixed 50-digit follow-up extends the deterministic plan to 64 unique A
families and covers every one of the 32 Gray-code B values per A. One process
captures the cumulative A prefixes 1, 4, 16, and 64. The comparison command
builds once and launches one fresh process for each worker count:

```bash
./scripts/test.sh profile-siqs-cycle-density 1
./scripts/test.sh profile-siqs-cycle-density 2
./scripts/test.sh profile-siqs-cycle-density 4
./scripts/test.sh compare-siqs-cycle-density
```

The profile is a manual Release-only target. It is not a CTest test and is not
part of smoke, module, changed, or gate catalogs. Both commands reject
`--no-build`, explicit `--retry`, and non-Release build types.

A successful process writes exactly six non-empty stdout records in this order:

```text
GNFS_SIQS_MULTI_A_CYCLE_CONFIG_V2
GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2 prefix_a=1
GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2 prefix_a=4
GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2 prefix_a=16
GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2 prefix_a=64
GNFS_SIQS_MULTI_A_CYCLE_SUMMARY_V2
```

Each record type has a closed schema. Missing, duplicate, empty, or unknown
fields fail validation. The runner also checks the prefix plan and slot counts,
capture and adapter conservation, graph rank, cycle density, assembly
conservation, stage deltas, and the CONFIG-to-PREFIX plan-digest bindings. A
non-zero process exit, timeout, partial transcript, blank record, missing final
newline, or successful process with stderr output is a failure.

Cross-worker identity excludes only the three validated worker fields, the
four wall-time fields, and the six current/peak RSS measurements. Those dynamic
fields remain format-checked. Every other CONFIG, PREFIX, and SUMMARY field must
match across the one-, two-, and four-worker fresh processes.

The reproduced fixed profile completed all 2048 logical slots without capacity
truncation. It first observed graph cycles at 16 A and cycles containing an
accepted 2LP source at 64 A:

| A Prefix | Captured Relations | Graph Edges | Graph Cycles | 2LP-Bearing Cycles | Selected Rows |
|---:|---:|---:|---:|---:|---:|
| 1 | 70 | 52 | 0 | 0 | 6 |
| 4 | 258 | 179 | 0 | 0 | 16 |
| 16 | 1113 | 762 | 2 | 0 | 84 |
| 64 | 4398 | 2951 | 57 | 5 | 397 |

All 57 cycles materialized successfully and none was rejected. The five
2LP-bearing cycles collectively reference sources from 15 A families, while
their accepted 2LP edges come from five A families. Those counts are reported
separately, so parallel 1LP edges cannot masquerade as 2LP cycle evidence. The
fixed plan used 64 planner attempts for 64 accepted A values, with zero
accepted duplicate A values. Its checked worst-case logical bounds are 65536
relations and 128MiB of relation payload, while the reproduced corpus retained
4398 relations and did not hit either per-slot limit.

The profile records `promotion=false solver_attempted=false`. These results are
cycle-density and materialization evidence, not a production promotion or
matrix-solver claim. In particular, 397 selected rows are well below the
50-digit target shape of 1701 rows (1601 factor-base columns plus 100 excess
rows). The next scale experiment should extend the same serial A plan to a
bounded 256-A prefix before any collector or `factor()` integration is
considered. That experiment still cannot replace the 70- and 90-digit gates or
the direct sparse-backend work.

## Current Fixed-Plan Evidence

The following evidence was reproduced on 2026-07-23 with one, two, and four
actual capture workers. All three worker records in each band had identical
plan, slot-state, raw-corpus, source, pre-trim, and selected-row digests. Every
slot completed without hitting its relation or payload cap.

| Band | Plan Digest Low:High | Raw Full / 1LP / 2LP | Accepted 2LP / Rejected | Graph Cycles | Selected Rows |
|---:|---:|---:|---:|---:|---:|
| 50 | `11016941208907243574:11284256310490571374` | 2 / 16 / 6 | 3 / 3 | 0 | 2 |
| 70 | `3984091375373043499:5485512563116088663` | 0 / 3 / 0 | 0 / 0 | 0 | 0 |
| 90 | `11074722052958763298:5003813898734258881` | 1 / 0 / 0 | 0 / 0 | 0 | 1 |

All three rejected 50-digit candidates were typed
`invalid_two_large_prime_split`. No band produced a large-prime graph cycle.
The records therefore validate bounded collection, typed normalization,
conservation, and worker-count independence, but they do not satisfy the 2LP
promotion gate or freeze an assembly/elimination parallel threshold.

Logical limits are frozen in the versioned fixture and emitted in every line.
A future JSON artifact will become the source of truth for persisted runs.

## Persistent Artifact Lifecycle Target

Artifacts live only under an ignored build directory. The runner must reject a
non-empty output directory, write each file to a sibling temporary path, flush
and close it, and then rename it atomically to its final name. Artifact commit
occurs only after every logical B slot and every requested shadow stage
succeeds. Cancellation or exception leaves no committed artifact. The runner
must never write generated capture data under `tests/`, `docs/`, or the
repository root.

The default artifact set contains the JSON summary and fingerprints only. Raw
relation retention requires an explicit option and remains subject to the same
logical payload cap. Artifacts contain no credentials, hostnames, usernames,
or absolute paths.

Keep all one-, two-, and four-worker artifacts until the comparison process
finishes. A reviewed comparison summary may inform a later committed design
update, but generated JSON and raw corpora remain untracked. Removing the build
directory removes the artifacts; the fixed input, plan, seed, schema, and limits
must make them reproducible.
