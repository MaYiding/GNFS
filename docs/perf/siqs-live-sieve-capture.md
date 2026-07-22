# SIQS Live-Sieve Capture Contract

## Status and Scope

This document defines an implementation target for bounded live-sieve evidence
for the Self-Initializing Quadratic Sieve (SIQS) two-large-prime (2LP) shadow
path. It does not report measurements, describe an existing command, or
authorize production 2LP collection.

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
5. Enumerate the requested Gray-code B ordinals in ascending order.
6. Hash the input, parameters, factor base, A definitions, B identifiers, and
   capture limits into `plan_fingerprint`.

Workers receive fixed contiguous ranges of logical B identifiers. They must not
advance a shared random-number generator, claim work through a race-dependent
queue, or stop after a shared first-arrival relation count. Each logical B slot
owns its output and applies the same local limits. The runner joins all slots in
logical B order before canonicalization.

Runs with one, two, and four workers must therefore consume the same planned
corpus. A worker-count comparison is invalid unless all three artifacts have
the same `plan_fingerprint`, planned B count, completed B set, truncation set,
and canonical source fingerprint.

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

| Band | Primary FB Size | FB Columns With Sign | Worker Counts | Required Outcome |
|---:|---:|---:|:---:|---|
| 50 digits | 1600 | 1601 | 1, 2, 4 | Complete bounded capture and conservation audit |
| 70 digits | 15000 | 15001 | 1, 2, 4 | Complete bounded capture and conservation audit |
| 90 digits | 130000 | 130001 | 1, 2, 4 | Complete bounded capture and sparse-backend boundary audit |

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

The capture runner must not call the legacy `merge_partials()` path. It must
pass the bounded raw corpus through these boundaries:

```text
prepare_two_large_prime_corpus
build_two_large_prime_cycle_basis
materialize_two_large_prime_cycle
assemble_siqs_shadow_rows
solve_siqs_shadow_matrix (only when its existing resource gates admit the shape)
verify_siqs_post_merge_dependency
extract_siqs_post_merge_factor
```

`normalize_two_large_prime()` remains the only admission boundary for an
unresolved candidate split. A failed split, a non-prime endpoint, an endpoint
above the large-prime bound, or an inexact product contributes to adapter
rejection statistics and never enters the graph.

## Artifact Schema

Each fresh process writes one versioned JSON summary. An optional bounded raw
corpus file may accompany it. The summary schema is exact for version 1:

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
stop-reason, graph, assembly, and fingerprint fields must match. Only
`plan.workers`, `informational.wall_nanoseconds`, and
`informational.peak_rss_bytes` may differ.

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

## Runner Interface Target

The following commands are implementation targets. They do not exist at the
time this contract is written.

```bash
# Implementation target only.
./scripts/test.sh bench-siqs-live-capture --band 50 --workers 1 --output-dir build-release/artifacts/siqs-live-sieve-capture/run-50-w1
./scripts/test.sh bench-siqs-live-capture --band 50 --workers 2 --output-dir build-release/artifacts/siqs-live-sieve-capture/run-50-w2
./scripts/test.sh bench-siqs-live-capture --band 50 --workers 4 --output-dir build-release/artifacts/siqs-live-sieve-capture/run-50-w4

# Implementation target only. Repeat the same fresh-process matrix for bands 70 and 90.
./scripts/test.sh compare-siqs-live-capture --input-dir build-release/artifacts/siqs-live-sieve-capture
```

The implementation must expose all logical limits as explicit command
arguments or a checked, versioned fixture. The JSON artifact remains the source
of truth for the effective values.

## Artifact Lifecycle

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
