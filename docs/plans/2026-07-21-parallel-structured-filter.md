# Deterministic Parallel Structured Relation Reduction

## Status

- Date: 2026-07-21
- Branch: `codex/parallel-structured-filter`
- State: M1 contracts and routing complete; M2a 2-way, M2b weight-[3,8]
  tree-basis, M2c budgeted sequential orchestration, and M3a.1 immutable
  conflict planning complete; M3a.2 parallel preparation is complete, and
  atomic batch publication is next
- Target: unify relation reduction and replace heuristic large-prime chain merging on large inputs with controlled structured Gaussian elimination over GF(2)

## Outcome

Build one deterministic relation-reduction engine for every production and test path, then add an opt-in structured large-prime filter that performs bounded `k`-way elimination in conflict-free parallel batches.

The development succeeds only when all of the following hold:

1. The adaptive sieve loop, distributed path, public `Pipeline::filter()`, stress/progressive drivers, and final pipeline use the same reduction policy and accounting.
2. Large-prime identity and odd-exponent parity are computed once, at full width, and agree bit-for-bit with `MatrixBuilder`.
3. Out-of-core snapshots do not finalize the writer; adding relations after a snapshot is defined and tested.
4. Every accepted elimination induces a bijection between the reduced left-dependency space and the original left-dependency space, while preserving exact source-relation provenance.
5. Weight-3 and higher LP columns produce `k - 1` source-coordinate-independent combinations instead of one BFS accumulator and an arbitrary residue.
6. One-thread and multi-thread runs produce identical rows, order, statistics, and stop reason.
7. A real overlapping 50-digit-like hypergraph preserves dependency dimension and materially improves full-matrix nonzeros, downstream matrix time, or required raw-relation count under frozen fill and memory budgets.
8. The 17/27/40/81-bit gate stays unchanged, a true 100-150-bit relation path passes, and a bounded 50-digit experiment confirms or rejects the structural gain.

## Why This Is Next

The project already has the end-to-end GNFS pipeline, out-of-core relation storage, checkpoints, distributed sieve workers, sparse matrix reduction, and thin-matrix Block Wiedemann. The current 50-digit evidence instead points to a relation-structure plateau: 282,027 usable rows versus 365,516 columns, with weight-3 and higher LP keys still dominating the unresolved graph.

The existing merge paths do not implement general structured elimination:

- `PartialRelationMerger::merge_all()` handles weight-2 columns and may consume only one pair from a weight-3 column. The independent survivor is not constructed.
- `CliqueRelationMerger::merge_cliques()` builds one BFS accumulator per walk. It may compress a component to one row and may emit a residue that is not a rank-preserving `k - 1` basis.
- The existing “50d synthetic” generator assigns relations to sequential LP keys. It does not create the overlapping 2LP/3LP/4LP hypergraph needed to measure fill or elimination quality.

The code audit also found six prerequisite contract groups that must be fixed before the new algorithm can be trusted:

1. `Pipeline::run()` calls `sieve_and_collect()` and then `solve_matrix()` directly. It does not call the public `Pipeline::filter()`, while adaptive, distributed, stress, and progressive paths each contain similar but drifting reduction logic.
2. The adaptive loop calls `RelationCollector::get_relations()` after each round. In OOC mode that method closes the writer, yet later rounds continue calling `add()`, which the collector contract marks undefined.
3. `PartialRelationMerger::remaining_lp_keys()` counts entries instead of exponent parity, while LP metrics and algebraic-square-root validation pack algebraic `(p, r)` into 64 bits by discarding high bits of both fields. These paths can disagree with matrix construction.
4. Production paths deduplicate on a collision-prone signed-shift expression derived from `(a, b)`. Reapplying primary-pair deduplication to structured outputs would also discard distinct `k - 1` combinations that share their first source row.
5. Merged relations can carry more than 16 raw LP entries per side, while `Relation::deserialize()` rejects more than 16; furthermore, reconstructing only each source's primary `(a, b)` would lose nested `extra_ab_pairs`.
6. A `RelationReductionResult` that owns only `std::vector<Relation>` cannot support the promised bounded-memory OOC route or keep the reduced corpus alive through matrix and square-root consumers.

Filtering literature models merge as the first stage of Gaussian elimination. Eliminating a column of weight `k` replaces its `k` rows with `k - 1` independent combinations and chooses combinations that limit fill. This is the missing abstraction, but it must be built on a single routing, parity, provenance, and persistence contract.

## Confirmed Premises

Confirmed by the user on 2026-07-21 as standing authorization for ordinary, reversible GNFS development decisions.

1. The main target is the forced GNFS path at 50 digits and above. SIQS remains the preferred automatic method below the current selection boundary.
2. This project improves relation reduction and matrix shape. It does not retune polynomial selection, factor-base bounds, special-Q scheduling, or cofactor yield.
3. The first release is opt-in. No default changes until cross-size, out-of-core, and bounded real-input evidence passes.
4. Exact GF(2) equivalence, source provenance, and deterministic output take priority over maximum thread utilization.
5. The V0 and V3 implementations remain as temporary baselines and fallback paths, but every caller reaches them through one reduction engine.
6. Correcting the existing routing, OOC snapshot, full-width LP-key, and serialization contracts is part of this development, not a separate cleanup project.

## Scope

### In Scope

- A canonical full-width odd-LP helper shared by filters, metrics, V0/V3, `MatrixBuilder`, and algebraic-square-root validation.
- A shared `RelationReductionEngine` used by every pipeline and test route.
- Appendable `RelationCollector` snapshots and an explicit consuming finalize operation.
- Reconciliation of the early “rows <= effective columns” return with the existing thin-matrix solver.
- Iterative singleton purge and controlled low-weight `k`-way LP elimination.
- Symmetric-difference source provenance and deterministic relation materialization.
- Exact raw-input identity, source-combination identity, nested provenance normalization, and a measured shared serialization contract for merged rows.
- Deterministic conflict-free parallel scoring and materialization.
- Release-active correctness checks, exact small-matrix oracles, realistic hypergraph generators, and route-regression tests.
- Opt-in policy, structured telemetry, documentation, and promotion criteria.

### Not in Scope

- Rewriting lattice sieving, polynomial selection, cofactorization, Block Wiedemann, or square root extraction.
- Enabling 3LP cofactorization by default. Its sieve-time cost needs a separate relation-yield study.
- Copying CADO-NFS source, file formats, or distributed filter executables.
- Making the structured path the default in its first implementation batch.
- Claiming a full 50-digit factorization from a synthetic benchmark or bounded first-round run.
- Converting every historical test-suite `assert()` in this project. New and modified relation tests must use always-on checks.

## Phase 0: Contracts Before Algorithms

### One Reduction Route and Ownership Boundary

Introduce `RelationReductionEngine` as the only owner of deduplication, canonical LP incidence, singleton policy, merge-strategy selection, metrics, and result validation.

```text
RelationCollector snapshot/finalize
                |
                v
       RelationReductionEngine
         |       |        |
         |       |        +--> structured strategy
         |       +-----------> V0/V3 baseline strategy
         +-------------------> no-LP strategy
                |
                v
       RelationReductionResult
         |       |        |
         |       |        +--> stop reason and policy decision
         |       +-----------> canonical rows/LP columns/excess
         +-------------------> active relations for MatrixBuilder
```

The following callers delegate to this engine instead of duplicating reduction logic:

- each adaptive sieve snapshot;
- the final `sieve_and_collect()` result;
- distributed collection;
- public `Pipeline::filter()`;
- stress, progressive, regression, and benchmark drivers.

The engine consumes a move-only `RawRelationSnapshot{generation, corpus}` and returns a distinct `RelationReductionResult`; the raw snapshot and reduced result are intentionally not interchangeable. A public vector entrypoint constructs a fresh generation explicitly. This type boundary, rather than a convention-only `reduced_once` flag, prevents accidental double reduction.

The shared storage boundary is:

```text
RelationCorpus
  = InMemoryCorpus(vector<Relation>)
  | OwnedOOCCorpus(store paths, format descriptor, cleanup owner)

RelationReductionResult
  - owns one RelationCorpus
  - exposes count/read and RelationSource iteration
  - keeps the corpus alive through MatrixBuilder and square-root dependency use
```

The reducer writes through a `RelationSink`; an OOC sink produces a new immutable reduced corpus. Matrix construction reads the corpus directly. Trimming uses deterministic selected indices or rewrites a reduced corpus. `MatrixResult` retains the corpus owner and dependencies read only selected relations. No milestone may call an O(output rows) vector “bounded memory.”

### Appendable OOC Snapshots

Split the collector API into two explicit operations:

- `snapshot_relations()`: returns a stable prefix and leaves later `add()` calls valid;
- `finalize_relations()`: consumes/finalizes the collector and forbids later `add()` calls.

For OOC mode, add an explicit writer state machine `Open -> Suspended -> Open`, `Open -> Finalized`, and any I/O failure to `Failed`. `write()` is legal only in `Open`; rejected writes never increment count. `finalize()` is idempotent and is the only operation allowed to publish final `MAGIC`.

`checkpoint_prefix()` returns a descriptor containing `store_id`, `generation`, committed `count`, and `data_end`. Under the collector lock it flushes and checks both streams, writes a temporary sentinel while keeping `MAGIC_INCOMPLETE`, closes writer handles, and then permits an explicit trusted-prefix reader to materialize that descriptor. After the reader is destroyed and all mappings are closed, the writer removes the temporary sentinel, restores the append cursors, and returns to `Open`. This strict close/read/unmap/reopen sequence is required on every platform, including Windows; the implementation must not rely on permissive POSIX sharing behavior.

The ordinary reader continues to reject incomplete stores. The trusted-prefix reader validates descriptor identity, overflow-safe index size, monotonic offsets, `offset[count] == data_end`, file bounds, exact deserialization consumption, and runtime bounds in Release builds. Recovery validates the last committed prefix, closes every handle, truncates uncommitted tails to `count/data_end`, and only then reopens append mode.

`SieveCheckpoint` moves to a versioned contract that records the OOC format version, store ID, generation, relation count, and data end. Save order is relation-prefix commit first, then a checkpoint containing that descriptor and the special-Q position. A V1 checkpoint cannot prove relation/SQ consistency and is rejected for automatic resume. Normal completion finalizes the relation corpus before deleting the sieve checkpoint. Tests use child-process `std::_Exit()` failpoints rather than “finalize then flip MAGIC” simulations.

Phase 0 freezes this lifecycle and corpus contract. The first functional snapshot may synchronously materialize a vector, but default promotion remains blocked until the reducer, MatrixBuilder, trimming, and dependency retrieval operate through the bounded-memory corpus/source path.

### Thin-Matrix Ownership

`solve_matrix()` already contains a thin-matrix Block Wiedemann path, but the earlier `Pipeline::run()` row/column check can make it unreachable. Keep the adaptive loop's positive-excess target as a collection heuristic, but make `solve_matrix()` the sole final authority after the special-Q budget is exhausted. The early check becomes a diagnostic, not a terminal return. Add a regression proving that a bounded thin result reaches the thin solver exactly once.

## Existing Components and Decisions

| Need | Existing component | Decision |
|---|---|---|
| LP identity | `LargePrimeKey` and `LargePrimeKeyHash` | Move to neutral `relation/large_prime_key.hpp`; keep all 64 bits of `prime` and `root` plus the side |
| Odd parity | `remaining_lp_keys()` plus MatrixBuilder-local logic | Replace with one sorted `odd_large_prime_keys()` helper that sums `PrimePower::e mod 2` |
| LP metrics | `count_unique_lp_keys()` and `count_lp_key_weights()` | Reimplement on canonical keys; remove `(p << 32) | low32(r)` packing |
| Row composition | `PartialRelationMerger::merge_two()` | Retain as a baseline helper; structured code composes source-ID sets and materializes canonically |
| Initial deduplication | collector and caller-local `filter_duplicates()` variants | Deduplicate raw rows once, before source-ID assignment, with full `ABPair`; never deduplicate structured rows by primary `(a,b)` |
| Size gating | `decide_v0_bfs_policy()` | Follow its explicit truth-table pattern; do not overload V0 semantics |
| Work scheduling | `gnfs::util::ThreadPool` | Reuse only after conflict batches are frozen deterministically |
| Large-input storage | `RelationCollector`, `OOCRelationStore`, and relation sources | Add appendable snapshot and streaming-reduction contracts |
| Matrix correctness oracle | existing sparse GF(2) SGE/row operations | Reuse as an independent small-case oracle, not as production filter code |
| Build/test catalog | `CMakeLists.txt` and `scripts/test.sh` | Register each binary, label, timeout, tier, module, and slow mapping |

## Proposed Structured Core

### Core Types

```cpp
using SourceRelationId = uint64_t;

struct RelationCombination {
    std::vector<SourceRelationId> source_ids;  // sorted symmetric difference
};

struct StructuredFilterConfig {
    size_t max_column_weight = 8;
    size_t max_constituent_relations = 64;
    size_t max_odd_lp_weight = 64;
    size_t max_serialized_lp_entries_per_side = measured_format_limit;
    int64_t max_total_lp_fill_delta = 0;
    int64_t max_total_matrix_fill_delta = measured_budget;
    size_t max_batch_candidates = 1024;
    size_t threads = 1;
};

enum class StructuredFilterStopReason {
    NotStarted,
    NoCandidates,
    ObjectiveReached,
    FillBudgetReached,
    RelationCapReached,
};

struct StructuredFilterStats {
    size_t input_rows = 0;
    size_t input_lp_columns = 0;
    size_t singleton_rows_removed = 0;
    size_t singleton_columns_removed = 0;
    size_t columns_eliminated = 0;
    size_t two_way_merges = 0;
    size_t higher_way_merges = 0;
    size_t fill_rejections = 0;
    size_t serialization_rejections = 0;
    size_t conflict_batches = 0;
    size_t peak_odd_lp_weight = 0;
    size_t output_rows = 0;
    size_t output_lp_columns = 0;
    StructuredFilterStopReason stop_reason{};
};

struct RelationReductionResult {
    RelationCorpus corpus;
    StructuredFilterStats structured_stats;
    size_t matrix_base_columns = 0;
    size_t lp_columns = 0;
    int64_t excess = 0;
    uint64_t input_generation = 0;
};
```

Invariant violations are errors and never normal stop reasons. The numeric caps are placeholders until M0 records current relation sizes, exponent totals, serialization counts, and matrix nonzeros. Configuration must validate that its persisted-entry cap is no greater than the single format maximum.

### Canonical LP Incidence

`odd_large_prime_keys(Relation)` must:

1. distinguish rational `p` from algebraic `(p, r)`;
2. preserve the full 64-bit `p` and `r` fields;
3. sum each `PrimePower::e` modulo two, rather than count vector entries;
4. return sorted, unique keys for deterministic hashing and comparison.

The helper lives in `include/gnfs/relation/large_prime_key.hpp` and exposes a visitor plus vector/count/empty wrappers. It uses XOR of `(e & 1)` and emits rational keys as `{p, 0, rational}` regardless of an input root. A stack fast path handles the common small relation and a hash fallback handles larger inputs; both sort the final unique keys.

`count_unique_lp_keys()`, `count_lp_key_weights()`, singleton logic, V0/V3 classification, structured incidence, both MatrixBuilder collection and row-building paths, and `verify_algebraic_ideal_powers()` consume this contract. `Relation::is_full()` and `num_large_primes()` keep raw-storage semantics, with comments prohibiting their use as GF(2) completeness predicates. Tests include even exponents, repeated entries, side collisions, two algebraic roots, and `p`/`r` values above `2^32` that collide under the old packing.

Raw relation identity is a complete `ABPair` comparison and hash. Remove every production `a ^ (b << 32)` identity, including adaptive and distributed paths; besides collisions, the signed shift is not a valid portable key. Raw deduplication happens once before immutable source IDs are assigned. A structured row is identified only by its sorted source-ID combination, and never by its materialized primary `(a,b)`.

### Provenance and Materialization

Each input row receives an immutable source ID. A structured row stores the sorted symmetric difference of its source IDs. Merging two rows performs a set XOR, so duplicate sources cancel and no source appears twice in the canonical combination.

Materialization rebuilds a `Relation` from immutable source rows, including sources that are already merged:

- flatten each selected source's primary `(a,b)` followed by all of its existing `extra_ab_pairs`; the first flattened pair becomes primary and every remaining pair becomes `extra_ab_pairs` in stable source/pair order;
- factor lists preserve every selected source contribution;
- rational LP entries consolidate by full `p` and rational side; algebraic entries consolidate by full `(p,r)` and algebraic side;
- totals larger than `PrimePower::e` can represent are emitted as deterministic bounded chunks instead of overflowing;
- no side may exceed the shared persisted-entry limit.

Move the persistence limit and format version to named relation-format constants used by stream and OOC readers/writers. M0 measures a defensible limit; it must not assume 256 while current readers enforce 16. A candidate that would exceed the constituent, odd-weight, exponent, or persisted-entry cap is rejected before incidence mutation.

Every accepted deep merge must pass different-parenthesization, `serialize -> deserialize -> MatrixBuilder`, OOC round-trip, and rational/algebraic square-root reconstruction tests. In-memory success alone is insufficient.

### Incidence Model

Maintain synchronized views:

- active row ID to sorted odd LP keys and source combination;
- LP key to sorted active row IDs.

Each row and column has a generation counter. Candidate-queue entries include the observed generation. Stale entries are discarded when popped, avoiding global heap rewrites after every commit.

### Singleton Fixed Point

A weight-one LP column cannot participate in a dependency. Remove its only row, update every incident column, and enqueue newly created singletons until a fixed point.

Removing one row can delete the pivot singleton column and any other column that becomes empty. Therefore LP-only excess `rows - active_lp_columns` is non-decreasing; it is not necessarily unchanged. Tests compare the fixed point to an exact peeling oracle.

### Controlled `k`-Way Elimination

For a pivot LP column with active rows `r[0..k-1]`:

1. Reject if `k < 2`, `k > max_column_weight`, or any proposed output exceeds a cap.
2. For `k == 2`, emit `r0 XOR r1`.
3. For `k >= 3`, score every pair from canonical key/source metadata without materializing candidate relations. The comparator is total and frozen in M0: projected full-matrix fill delta, LP fill delta, source count, persisted factor-entry count, then stable row IDs.
4. Select a minimum spanning tree over the `k` rows.
5. Emit one source combination per tree edge.
6. Replace the `k` inputs with the `k - 1` outputs during ordered commit.

Let `M` be the original relation matrix, `T` the reduced-row to immutable-source-row transform, and `F = T M` after eliminated pivot columns are removed. For a weight-`k` pivot, the selected tree incidence has rank `k - 1`; its transpose maps reduced left dependencies back to source coordinates. The pivot column forces every original dependency on those rows to have even parity, and the image of a tree-incidence transpose is exactly that even-parity subspace. Therefore `ker(F^T) -> ker(M^T)` under `T^T` is a bijection. This dependency-space isomorphism—not equality of row spaces—is the correctness contract.

The oracle verifies `rank(T)`, equality of dependency dimensions, forward dependency mapping, and that each materialized source combination equals its row of `T`. Its payload includes sign, rational/algebraic factor-base, quadratic-character, Schirokauer, and LP columns rather than an LP-only graph. A legal pivot does not manufacture nullity or true matrix excess; rows/columns, nonzeros, fill, solver time, and preserved dependency dimension are reported separately.

Each accepted elimination removes one active row and the pivot column before incidental cancellations. Other columns remain represented exactly; when no candidate is admissible, all active rows pass to `MatrixBuilder` with their canonical remaining LP columns. The structured path never emits a one-off BFS accumulator as a substitute for the active basis. Only selected tree edges are materialized.

Implement a simple pivot planner as the reference and the MST planner as the production choice. The exact oracle must show that both induce the same dependency-space isomorphism, even when their reduced row bases differ.

### Deterministic Parallelism

Introduce parallelism in four bounded layers:

1. Build per-worker incidence shards and merge them in sorted LP-key order.
2. Score candidate columns in parallel against an immutable generation snapshot.
3. Build a batch greedily in globally sorted candidate order. A batch contains only candidates whose input rows are disjoint.
4. Materialize candidates in parallel, then publish one batch in original sorted order.

The first release does not update shared incidence maps concurrently. This keeps the commit sequence deterministic while parallelizing parity extraction, candidate cost calculation, and relation materialization.

M3a preserves the sequential reference order: all 2-way candidates precede
tree-basis candidates, and each kind retains its existing total order. The
planner canonicalizes scorer output, removes equivalent same-member plans,
then builds a greedy maximal member-disjoint set. This is not the future full
`MatrixBuilder` score and is not a maximum-cardinality set-packing solver.

M3a.2 seals and validates the reducer once on the coordinator thread, validates
exact plans in candidate order, and dispatches only pure corpus materialization.
The barrier retains `PersistenceLimit` as a per-slot outcome. It drains every
submitted future before rethrowing any other error, and the lowest candidate
index determines which error escapes. The `threads=1` path follows the same
attempt-all and ordered-error contract without creating a thread pool.

Batch execution will transform every selected member set against one frozen
snapshot, publish the batch atomically with one epoch advance, and peel
singletons after the whole batch. `threads=1` defines the batch-scheduler
reference. M3 does not claim byte-for-byte row equivalence with the M2 loop,
which peels after every individual commit.

`parallel_merge_partials()` cannot be the main scheduler because relations appear in multiple LP buckets. Buckets are not independent; explicit row-conflict detection is mandatory.

## Runtime Policy

Add one relation-module mode:

```text
GNFS_STRUCTURED_FILTER=0|1|auto
```

- unset or `0`: preserve the legacy strategy selected by the existing size-aware V0/V3 policy.
- `1`: force the structured strategy on supported inputs; unsupported inputs or invariant errors fail explicitly and never silently fall back.
- `auto`: explicitly request the size policy; supported inputs may select structured mode, unsupported inputs use the named legacy strategy, and invariant errors fail explicitly.
- invalid: reject configuration before consuming a snapshot.

Promotion later changes only the unset default from OFF to size-aware auto. A table-driven truth table covers unset/0/1/auto/invalid × supported/unsupported × normal/no-candidate/invariant-error. `NoCandidates` is a successful unchanged structured result; it is not a hidden fallback.

Production uses `ThreadPool(0)` hardware selection because pipeline thread configuration no longer exists. `StructuredFilterConfig::threads` is only an explicit library/test override; zero hardware concurrency falls back to one worker. Cross-thread determinism is required on a fixed platform and build. Cross-platform byte-level column ordering additionally requires sorting canonical LP keys before column assignment and is a separate verified criterion.

Keep `GNFS_V0_BFS` and `GNFS_CASCADE_V3` for baseline comparison. `RelationReductionEngine` selects exactly one strategy for a snapshot; structured mode never consumes V0/V3 output a second time. Emit one stable record containing policy reason, input generation, rows, LP columns, excess, nonzeros, caps, thread count, and stop reason.

## Parallel Development Lanes

The implementation uses four isolated ownership lanes after M0 freezes shared contracts. Each lane works in a separate file set and integrates only at stated gates.

### Lane A: Canonical LP Contract and Consumer Sweep

Owns:

- `include/gnfs/relation/large_prime_key.hpp`;
- canonical-key changes in `filter.hpp`, `clique_merger.hpp`, `matrix_builder.hpp`, and `sqrt/algebraic_sqrt.hpp`;
- `test_lp_key_contract` and only its catalog entries.

Lane A lands the neutral helper API first. No other lane edits its consumer files until that commit is integrated.

### Lane B: OOC Lifecycle and Checkpoint Contract

Owns:

- `ooc_relation_store.hpp`, collector snapshot/finalize code, and `sieve_checkpoint.hpp`;
- OOC/checkpoint lifecycle tests and failpoint child cases;
- measured relation-format descriptor/constants after M0.

Lane B does not edit MatrixBuilder, filtering, or structured mathematics. The primary integrator owns the small pipeline save-order patch after Lane B's API commit.

### Lane C: Mathematical Core, Oracle, and Test Data

Owns:

- source-combination representation and materializer in new structured files;
- sequential incidence, singleton purge, pivot planner, and MST planner;
- an exact dense GF(2) rank/kernel oracle for small cases;
- source-ID accounting checks;
- a true overlapping LP-hypergraph generator with controlled row and column weights;
- V3 regression fixtures and structured property tests.

Tests use an always-on `CHECK` mechanism. Modified clique tests are converted away from bare `assert()` so Release validation is meaningful.

### Lane D: Routing, Parallel Scheduler, and Measurement

Owns:

- `RelationCorpus`, `RelationReductionEngine`, and mode public types after the M0 API commit;
- adaptive/distributed/public/stress/progressive routing and exact `ABPair` dedup sweep;
- thin-matrix handoff and mode parser;
- conflict-batch construction and worker scheduling after Lane A freezes the sequential API;
- shared build/test catalog entries after per-test lane entries land;
- algorithm/runtime documentation;
- baseline and comparison reports.

Lane D may add benchmark scaffolding early but does not alter core correctness code.

### Integration Gates

```text
M0 contract freeze
      |
      +--> Lane A core ---------+
      +--> Lane B routing ------+--> sequential oracle gate
      +--> Lane C oracle -------+
      +--> Lane D scaffolding --+
                                  |
                                  +--> parallel scheduler gate
                                  |
                                  +--> cross-size and OOC gate
```

The primary integrator resolves shared-header changes, runs route-equivalence tests, and reviews the full diff. No lane edits another lane's owned file to “help” without an explicit handoff.

## Milestones

### M0: Baseline and Contract Freeze

- Configure a clean Release build without touching generated directories.
- Record V0 and V3 metrics, LP histograms, serialization entry counts, wall time, and peak RSS.
- Freeze `RelationReductionEngine`, canonical LP helper, source-combination, snapshot, and result contracts.
- Add a source-ID fixture that reproduces the current V3 residue/accounting risk.

Exit gate: fixed corpora, environment matrix, comparator/budgets, corpus ownership, persistence cap, dependency-space proof, dedup identities, nested provenance, and snapshot state machine are recorded; no production behavior changed.

### M1a: Canonical LP Identity and Classification

- Replace packed LP metrics and entry-count parity with the canonical helper.
- Route MatrixBuilder, metrics, V0/V3 classification, separation, collector/cofactor statistics, and algebraic-square-root validation through the helper.
- Sort LP keys before cross-platform column assignment.

Exit gate: high-bit/exponent/side tests, helper-to-MatrixBuilder agreement, V0/V3 classification, and square-root collision regressions pass.

### M1b: Collector and OOC State Machine

- Implement snapshot/finalize collector semantics and OOC lifecycle tests.
- Version sieve checkpoints and enforce relation-prefix-before-SQ commit order.
- Replace synthetic resume tests with child-process crash/failpoint tests.

Exit gate: snapshot-append-snapshot-finalize works; recovery sees only the last committed prefix; finalized/failed writers reject writes without count drift.

### M1c: Exact Raw Identity and One Reduction Route

- Remove packed/signed-shift `(a,b)` deduplication and deduplicate raw rows once with full `ABPair` before source-ID assignment.
- Introduce the move-only snapshot, corpus-owning reduction result, and shared reduction engine.
- Remove caller-local policy copies and prove the OFF-mode relation-reduction digest is identical on fixed fixtures.

Exit gate: collision and shared-primary-output regressions pass; every route reduces a generation exactly once; relation-reduction output is equivalent in OFF mode.

### M1d: Thin-Solver Handoff

- Make the thin solver reachable after collection budget exhaustion.
- Record this intentional end-to-end behavior change separately from OFF-mode reduction equivalence.

Exit gate: a bounded thin result reaches the existing thin solver exactly once and the previous early return is unreachable.

### M2: Sequential Structured Reference

- M2a, complete: build deterministic incidence, singleton fixed point, degree-2
  planning, immutable source XOR, nested materialization, persistence preflight,
  incidence epochs, transactional commit, and explicit stop accounting.
- M2a is intentionally vector-backed and keeps append-only tombstone history. Its
  exhaustive oracle covers factor, AB-provenance, and LP projections, including
  nested residual merges. It is not the complete `MatrixBuilder` or OOC oracle.
- M2b tree-basis core, complete: plan deterministic 3-way through 8-way pivots
  with both a reference star and an LP/source-sparsity-scored MST. Preparation
  materializes the complete tree without mutation; commit publishes contiguous
  output IDs only after all allocations succeed. Active source transforms use
  one full-row-rank invariant throughout M2a and M2b, so nested outputs may
  overlap in source IDs. Fixed fixtures and randomized cases check the exact
  even-parity tree span, dependency-kernel mapping, deterministic tie-breaks,
  stale and forged plans, and persistence-failure atomicity.
- M2c, complete: add per-invocation candidate-examination, commit, emitted-row,
  LP-fill, source, pivot-weight, output, and post-prepare materialization caps.
  The deterministic driver repeatedly plans 2-way and tree-basis candidates,
  skips cached persistence failures, commits the first admissible candidate,
  and peels singleton rows. Exact boundary tests freeze rejection precedence,
  stop reasons, commit-granular atomicity, cache fairness, deterministic output,
  full source rank, and dependency-kernel mapping.
- M2c remains a vector-backed sequential reference. It constructs every plan
  for an epoch, and its accepted-payload cap is not an allocation or peak-RSS
  bound. Parallel batches, bounded-memory OOC execution, and full
  `MatrixBuilder` payload equivalence remain later gates.
- Complete randomized cases and full sign, quadratic-character, Schirokauer,
  stream, and OOC round-trip validation before claiming the M2 exit gate.

Exit gate: every hand-built and randomized case passes the exact dependency-space and provenance oracle in one thread.

### M3: Deterministic Parallel Scheduler

- M3a.1, complete: expose immutable snapshot identity; canonicalize shuffled
  candidate vectors; verify same-member duplicate payloads; and select a
  deterministic greedy maximal batch using active member-row conflicts only.
  Exact fixtures freeze candidate order, duplicate representatives, width
  accounting, stale epochs, and maximal-not-maximum behavior.
- M3a.1 is planning-only and vector-backed. It constructs all 2-way and tree
  plans before applying batch width. It does not prepare, execute, budget, or
  commit a batch, and it does not claim bounded planning memory or parallel
  speedup.
- M3a.2, complete: add an opaque, move-only prepared batch; retain ordered
  success and persistence-limit slots; run exact validation once per selected
  candidate after one whole-state seal; and materialize candidates through a
  drain-all ordered parallel map. Controlled future gates freeze lowest-index
  error precedence and prove that the barrier does not return before tail work
  finishes.
- M3a.2 does not commit, peel, update statistics or persistence caches, apply a
  reduction budget, or permit concurrent reducer mutation. Tree edges inside
  one candidate remain sequential to avoid nested pools.
- M3b, next: add atomic batch commit, aggregate budgets and statistics, assign
  output IDs in sorted prefix order, advance the epoch once, and peel once
  after publication. Parallel shard construction and the complete scheduler
  loop follow that atomic boundary.
- Compare `threads=1,2,4,hardware_concurrency`.
- Run the narrow relation suite under ThreadSanitizer where supported.

Exit gate: result rows, order, stats, and stop reason are identical across thread counts, with no race report.

### M4: Opt-In Integration

- Add `GNFS_STRUCTURED_FILTER` parsing and exact strategy selection.
- Register tests and document the flag and fallback contract.
- Complete the `RelationSource`/`RelationSink` reducer, direct corpus MatrixBuilder input, deterministic corpus trimming, and selected-dependency retrieval.
- Validate adaptive, distributed, public, stress, progressive, and final routes against the same engine.

Exit gate: structured mode runs exactly once, OFF mode is unchanged, every route reports the same metrics for the same snapshot, and OOC memory stays bounded.

### M5: Scale Validation

- Run overlapping 50-digit-like hypergraphs at 5K, 50K, and 200K rows.
- Run the 17/27/40/81-bit gate.
- Run a deterministic 100-150-bit sieve-to-reduction integration, not only polynomial selection.
- Run a bounded 50-digit first-round experiment with structured metrics.

Exit gate: no correctness regression; dependency dimension is preserved and structured mode meets the frozen materiality threshold for total NNZ, downstream matrix time, or raw-relation requirement under the fill and memory budgets.

### M6: Promotion Decision

- Compare dependency dimension, rows, LP columns, nominal excess, total nonzeros, peak source count, filter wall time, peak RSS, downstream matrix time, and required raw relations.
- Promote default auto behavior only if every criterion below passes.
- Otherwise keep the implementation as an explicit research mode and document the measured limitation.

## Test Matrix

| Code path or branch | Test type | Required evidence |
|---|---|---|
| Canonical odd LP keys | Unit | `e mod 2`, duplicate entries, side/root identity, full 64-bit values |
| Matrix/helper agreement | Unit + property | Helper LP columns exactly match `MatrixBuilder` columns |
| Filter/sqrt consumer agreement | Unit + regression | V0/V3/full classification and algebraic-sqrt verification use the same full-width parity |
| Raw/structured identity | Regression | Full `ABPair` collision resistance and two shared-primary structured outputs both survive |
| Nested provenance | Unit + integration | Merged-plus-raw input, parenthesization, stream/OOC, matrix row, and sqrt reconstruction agree |
| OOC snapshot lifecycle | Integration + child process | Snapshot, append, second snapshot, failpoint crash, committed-prefix resume, finalize, no data loss |
| Corpus ownership | Integration | Vector and OOC results outlive reducer locals; MatrixBuilder and selected dependency reads stay valid |
| Shared route selection | Integration | Same snapshot and mode produce identical output from every caller |
| Thin final handoff | Integration | Exhausted thin collection reaches BW path once instead of returning early |
| V3 source accounting | Regression | At least two sources per emitted merge, no duplicate source IDs, exact XOR |
| Singleton fixed point | Unit + property | Peeling matches the exact oracle; excess never decreases |
| 2-way elimination | Unit | Pivot gone, source XOR exact, provenance materializes correctly |
| `k`-way MST | Unit + property | Exactly `k - 1` source-coordinate-independent outputs, pivot absent, and dependency mapping bijective |
| Incidental cancellation | Unit | Actual fill score matches canonical output keys |
| Caps and stale candidates | Unit | Rejection is mutation-free; stale generations cannot double-consume rows |
| Serialization depth | Integration | Measured cap enforced; deep/nested merge survives stream and OOC round trips, MatrixBuilder, and sqrt reconstruction |
| Conflict-free batch | Unit | Overlapping candidates never enter the same batch |
| Thread equivalence | Unit + integration | Relations, order, metrics, and stop reason are byte-equivalent |
| Realistic hypergraph | Scale | Configured 2/3/4+ LP row overlap, dependency dimension, full NNZ fill, and histogram, not sequential one-key rows |
| Mode OFF | Integration | Existing V0/V3 result remains unchanged |
| Mode ON | Integration | Structured strategy selected once; no fallback double consumption |
| 17/27/40/81-bit path | Gate | Existing factorization checks pass |
| 100-150-bit relation path | Heavy targeted | Real sieve-to-reduction size transition passes |
| Bounded 50-digit round | Experiment | Structural evidence supports or rejects promotion |

The new 100-150-bit test must exercise actual relation collection and reduction. `test_kleinjung_large` remains a useful polynomial-selection companion but is not accepted as the relation-path gate.

## Validation Commands

Run from narrowest to widest:

```bash
./scripts/test.sh build
./scripts/test.sh run test_lp_key_contract
./scripts/test.sh run test_relation_collector_snapshot
./scripts/test.sh run test_relation_reduction_routes
./scripts/test.sh run test_structured_filter
./scripts/test.sh run test_structured_filter_property
./scripts/test.sh run test_structured_filter_parallel
./scripts/test.sh run test_structured_filter_50d_synthetic
./scripts/test.sh module relation
./scripts/test.sh changed --deep
./scripts/test.sh gate
./scripts/test.sh run test_structured_filter_pipeline_120bit
./scripts/test.sh run test_kleinjung_large
./scripts/test.sh list
```

The exact binary names are finalized in M0 and then registered in the live catalog. The plan does not assume they exist before implementation.

The bounded 50-digit run records:

- raw, deduplicated, active, singleton-removed, and output row counts;
- full LP column-weight histogram and row LP-weight percentiles;
- base columns, LP columns, total columns, and excess before and after reduction;
- total factor/LP nonzeros and fill delta;
- source-count and persisted-LP-entry percentiles and maxima;
- filter wall time, peak RSS, worker count, batch occupancy, and speedup;
- strategy, policy reason, cap rejections, and stop reason.

A first-round run is not a full factorization claim. A full stress run is justified only if bounded evidence shows a credible route to positive excess or a materially smaller matrix.

## Performance Budgets

M0 replaces these initial budgets with measured baselines before promotion:

- Inputs below the size gate: no structured work and less than 1% OFF-mode overhead.
- 50K overlapping synthetic rows: stay within the relation test's declared Release timeout.
- 200K rows: peak memory bounded by source storage, incidence, and one output batch; no obsolete-row history.
- Four workers: at least 1.5x speedup in scoring plus materialization on a workload large enough to amortize scheduling.
- Ordered commit: less than 35% of structured-filter wall time at 200K rows; otherwise redesign batch size before adding shared-map concurrency.
- Fill, source count, serialized LP entries, and exponent chunks: configured, reported, and never silently exceeded.
- OOC mode: no full duplicate of both raw and reduced corpora after streaming integration.

Matrix-quality improvement outranks microbenchmark speed. A slower filter may be acceptable only when measured downstream matrix work or required raw-relation count falls by more than the added cost.

## Failure Modes and Rescue

| Failure | Detection | Rescue |
|---|---|---|
| Canonical LP helper disagrees with MatrixBuilder | Full-width/parity property test | Block structured work; make MatrixBuilder consume the helper |
| OOC snapshot finalizes or loses append data | Snapshot-append-finalize integration | Use immutable round segments; do not keep undefined append behavior |
| A route applies reduction zero or two times | Generation/result-state assertion | Fail closed and route through the engine |
| Pivot survives or dependency mapping is not bijective | Invariant checker and exact oracle | Abort structured strategy with input snapshot untouched |
| Source appears twice or provenance is lost | Source-XOR and square-root reconstruction test | Rebuild from immutable source IDs; block rollout |
| Persisted relation exceeds reader contract | Precommit materialization check and round trip | Reject candidate or raise one shared bounded format limit with tests |
| Candidate consumes an old row | Generation and conflict assertion | Discard stale candidate and rescore |
| Fill grows too quickly | LP/full-matrix fill budgets plus source and persisted-entry caps | Reject candidate and continue |
| No admissible low-weight column | Explicit stop reason | Return the exact active basis and remaining LP columns |
| Parallel result differs | Thread-equivalence test | Disable parallel scheduler; keep sequential reference |
| Synthetic gain does not transfer | Real 120-bit and bounded 50-digit runs | Keep opt-in and revisit purge/sieve yield separately |
| Thin solver still unreachable | Route regression and phase telemetry | Remove remaining pre-solver terminal gate |

## Promotion Criteria

Unset `GNFS_STRUCTURED_FILTER` may become size-aware auto only when:

1. Canonical parity, provenance, dependency-space, serialization, OOC, and route-equivalence suites pass in Release.
2. One-thread and multi-thread results are identical on every deterministic suite.
3. Existing 17/27/40/81-bit results remain unchanged in OFF mode.
4. A true 100-150-bit relation path passes without relying on `test_kleinjung_large` alone.
5. The bounded 50-digit run meets the M0-frozen materiality threshold for full-matrix nonzeros, downstream matrix time, or projected raw-relation demand; nominal excess alone is insufficient.
6. Fill and downstream matrix nonzeros stay within the frozen budget.
7. OOC structured reduction is bounded-memory and snapshot append remains defined.
8. No tested size band regresses end-to-end wall time by more than 5% without a larger measured downstream gain.
9. Unsupported auto inputs have a tested named fallback; forced-mode unsupported inputs and invariant errors fail explicitly, while no-candidate completion has an explicit normal stop reason.

## Decisions Frozen by This Plan

1. The production `k >= 3` planner uses MST; a simpler pivot planner remains the independent reference oracle.
2. When elimination stops, the engine returns the exact active transformed basis and its remaining LP columns. It does not emit a heuristic accumulator residue.
3. Source provenance is a symmetric-difference set of immutable source IDs, not concatenated opaque merge chains.
4. All relation persistence paths share one measured, versioned LP-entry contract and one corpus ownership boundary.
5. Parallelism uses snapshot scoring, conflict-free batches, and ordered commit before considering concurrent incidence mutation.

## Completion Definition

The project is complete only when code, tests, documentation, route integration, and measured evidence agree. A header-only helper without the production caller does not count. Passing only the 81-bit gate does not count. A synthetic improvement without realistic overlap and exact dependency-space equivalence does not count. An in-memory result that fails after serialization or corpus-lifetime use does not count.

A bounded 50-digit result that disproves the approach is still a valid engineering outcome if the mode stays opt-in and the evidence, stop reason, and next decision are recorded.

## CEO Review Record

### Step 0A: Premise Challenge

| Premise | Evidence and challenge | Decision |
|---|---|---|
| Target 50-digit-and-larger GNFS | The recorded 50-digit matrix has negative excess and a weight-3+ LP plateau. Smaller automatic inputs already prefer SIQS. | Accepted. This is the highest-leverage unresolved GNFS path. |
| Improve reduction before yield | The current rows/columns gap is structural, but a filter cannot manufacture rank. The bounded experiment must be allowed to disprove the premise. | Accepted with a kill criterion: no structural gain sends future work back to sieve/cofactor yield. |
| Opt-in first | The path changes dependency-space transformations and persistence limits. Default-on would make rollback and cross-size diagnosis harder. | Accepted. Explicit flag first; unset behavior stays unchanged. |
| Determinism over saturation | Reproducibility is a project priority and conflict-free batches still expose useful parallel work. | Accepted. Ordered commit is not relaxed for benchmark gains. |
| Keep V0/V3 temporarily | They are required as baselines and rollback paths but should not become permanent duplicate architecture. | Accepted through M6; retirement becomes a measured post-promotion decision. |
| Contract fixes are part of scope | The current routing, OOC lifecycle, key packing, and serialization mismatches can invalidate any structured-filter conclusion. | Accepted as M1 blockers, not optional cleanup. |

Doing nothing leaves the 50-digit path collecting more raw relations without evidence that the reduced matrix becomes materially cheaper while preserving dependencies. A pure thread-level optimization would make the same structural failure arrive faster. The plan addresses the dependency-preserving reduction bottleneck directly and has a bounded experiment that can reject the bet.

### Step 0B: What Already Exists

| Sub-problem | Existing code | Reuse |
|---|---|---|
| LP semantic identity | `LargePrimeKey` in `filter.hpp` | Move the type to a neutral header; remove lossy metric and square-root packing. |
| Baseline parity and composition | `remaining_lp_keys()` and `merge_two()` | Use only as regression baselines; structured code uses the canonical helper and source IDs. |
| Matrix LP semantics | `MatrixBuilder` | Make it consume the same helper used by filtering and diagnostics. |
| Sequential low-weight elimination | relation V0 and linalg SGE | V0 supplies comparison cases; linalg SGE supplies an independent oracle. |
| Parallel execution | `gnfs::util::ThreadPool` | Reuse after deterministic batches are frozen. |
| Large relation persistence | `RelationCollector` and `OOCRelationStore` | Extend lifecycle; do not invent a second file format. |
| Size-aware policy | `decide_v0_bfs_policy()` | Reuse the table-driven policy pattern. |
| Test orchestration | `scripts/test.sh` | Keep one catalog and live tier metadata. |

The plan does not introduce an external filter executable, database, service, or third-party concurrency runtime.

### Step 0C: Dream State

```text
CURRENT
  duplicated reduction routes
  heuristic V0/V3 combinations
  parity/persistence drift
        |
        v
THIS PLAN
  one reduction engine
  exact low-weight structured elimination
  deterministic parallel batches
  full-width parity and bounded OOC contracts
        |
        v
12-MONTH IDEAL
  streaming, checkpointable relation-reduction platform
  policy selected from measured matrix cost
  reproducible traces across local/distributed execution
  V0/V3 retired after evidence-backed migration
```

This plan reaches the reusable engine and correctness substrate. It deliberately stops before a standalone distributed filter service or automatic yield tuning.

### Step 0C-bis: Implementation Alternatives

| Approach | Effort | Risk | Completeness | Advantages | Costs |
|---|---:|---:|---:|---|---|
| A. Patch V3 and LP metrics in place | M | Medium | 5/10 | Small diff; quick regression fix | Keeps route duplication, OOC undefined behavior, and no general `k`-way basis |
| B. Staged shared engine plus structured filter | XL | Medium | 10/10 | Fixes contracts first; reversible rollout; exact oracle; supports later streaming | More files and integration gates; requires disciplined ownership |
| C. Standalone CADO-style distributed filter subsystem | XXL | High | 10/10 | Strong long-term scale isolation | Duplicates file/distribution machinery and delays useful in-repo evidence |

Decision: Approach B. It has the same correctness coverage as the ideal external subsystem without spending a second infrastructure stack. Approach A cannot establish trustworthy 50-digit evidence.

### Step 0D: Selective Expansion Decisions

| Proposal | Decision | Rationale |
|---|---|---|
| Bounded-memory `RelationSource` reduction | Accepted | Required before OOC default promotion; already included in M4. |
| Stable reduction digest for replay/equivalence | Accepted | Small addition that makes route/thread determinism observable without storing every merge. |
| Standalone filter executable and distributed file protocol | Deferred | Useful only after the in-process algorithm proves matrix gain. |
| 3LP yield and cofactor retuning | Deferred | Different experiment; mixing it would hide whether reduction itself works. |
| Automatic V0/V3 deletion | Deferred | Remove only after promotion evidence and a compatibility window. |
| Performance dashboard/UI | Skipped | Stable logs and report artifacts answer the engineering need without UI scope. |

### Step 0E: Temporal Interrogation

| Implementation point | Decision frozen now |
|---|---|
| Foundations | One full-width helper owns parity; one reduction engine owns policy; snapshot and finalize are distinct operations. |
| Core logic | Source combinations use symmetric difference; MST is production, pivot planner is oracle; caps reject before mutation. |
| Integration | OFF relation-reduction digest is identical; the engine consumes each generation once; thin solving is owned by `solve_matrix()` and is a separately tested behavior fix. |
| Tests and scale | Release-active checks, real overlap, persistence round trips, route equivalence, and a real 100-150-bit relation path are mandatory. |

### Step 0F: Review Mode

Mode: selective expansion. The plan keeps the 50-digit reduction objective, accepts only streaming and reproducibility support, and defers yield tuning and external filter infrastructure.

### Section 1: Architecture Review

Three architectural findings were accepted:

1. Structural and behavioral changes must be sequenced. M1 unifies contracts and proves OFF-mode equivalence before M2 changes merge mathematics.
2. The collector and reducer need explicit state machines. A boolean “already reduced” convention is insufficient; a move-only raw snapshot and distinct result type enforce generation ownership at the engine boundary.
3. The input snapshot must remain immutable until a structured result validates. Candidate rejection is normal; an invariant error never returns a partially committed basis.

Full architecture:

```text
Special-Q workers / distributed shards
                |
                v
        RelationCollector
       COLLECTING state
          |          |
          | snapshot | finalize
          v          v
  immutable prefix   FINALIZED corpus
          |          |
          +----+-----+
               v
    RelationReductionEngine
       |        |        |
       |        |        +--> StructuredFilter
       |        +-----------> V0/V3 baseline
       +--------------------> no-LP pass-through
               |
               v
   validated ReductionResult
      rows + LP columns + digest
               |
               v
          MatrixBuilder
               |
               v
       SGE -> BL/BW -> sqrt
```

Collector state machine:

```text
              snapshot success
     +--------------------------------+
     |                                |
     v                                |
 COLLECTING --snapshot start--> SNAPSHOTTING
     |                                |
     | finalize                       +--snapshot error--> COLLECTING + visible error
     v
 FINALIZING --success--> FINALIZED
     |                       |
     +--error--> ERROR       +--add/snapshot/finalize--> rejected

 Invalid transitions never mutate files or counts.
```

Reduction transaction:

```text
IMMUTABLE INPUT
      |
      v
BUILD INCIDENCE -> SCORE/PLAN -> MATERIALIZE TEMP OUTPUT -> VALIDATE
      |                |                 |                   |
      +--invalid------>+--cap reject---->+--I/O/overflow---->+--mismatch
            |                 |                 |                   |
            +-----------------+-----------------+-------------------+
                                      |
                                      v
                          input remains authoritative
```

At 10x load, incidence memory and source-set materialization break before CPU. At 100x, repeated full OOC snapshots are unacceptable; direct corpus source/sink reduction, matrix input, trimming, and selected-dependency reads are therefore promotion blockers. No network or authorization boundary is added.

Rollback is `GNFS_STRUCTURED_FILTER=0` for behavior and a branch revert for contract changes. The OOC/checkpoint contract is explicitly versioned; old readers may reject deeper rows or newer headers, so producing version and persistence cap travel with owned corpora rather than being assumed compatible.

### Section 2: Error and Rescue Registry

Use one contextual `RelationReductionError` with an explicit code instead of catch-all exceptions:

```cpp
enum class RelationReductionErrorCode {
    InvalidInput,
    IncidenceMismatch,
    SourceCombinationInvalid,
    ExponentOverflow,
    PersistenceLimit,
    SnapshotIo,
    SnapshotState,
};
```

| Method/codepath | Failure | Code or exception | Rescue | Observable result |
|---|---|---|---|---|
| `odd_large_prime_keys()` | malformed exponent/key or allocation failure | `InvalidInput`, `std::bad_alloc` | Validate fields; propagate allocation failure | Reduction abort with relation index/key context |
| `snapshot_relations()` | flush/seek/sentinel/close/reopen failure | `SnapshotIo` | Recover last committed prefix if possible; otherwise writer enters `Failed` | Pipeline stops; writer path and generation logged |
| `snapshot_relations()` | snapshot after finalize or concurrent invalid transition | `SnapshotState` | Reject before mutation | Explicit lifecycle error |
| `RelationReductionEngine::reduce()` | already-consumed snapshot | Compile-time move-only boundary or explicit invalid-input error | Reject caller bug | Phase and generation logged; no second output |
| incidence build/commit | row/column views disagree | `IncidenceMismatch` | Discard temporary result | Structured path fails closed |
| source XOR/materialize | empty, duplicate, or unknown source ID | `SourceCombinationInvalid` | Reject candidate or abort if internal invariant | Source IDs and pivot logged |
| exponent normalization | sum cannot be represented in bounded chunks | `ExponentOverflow` | Reject before commit | Rejection counter; forced mode fails if no valid result |
| relation persistence | entry count exceeds shared limit | `PersistenceLimit` | Candidate rejection before incidence mutation | Limit and projected count logged |
| thread task | worker throws | original exception plus batch context | Join all workers; discard batch; rethrow | Batch ID and candidate keys logged |
| final validation | digest, pivot, or dependency-space invariant fails | `IncidenceMismatch` | Never publish result | Pipeline returns explicit failure |

Candidate-cap rejection and “no candidates” are normal stop reasons, not exceptions. `std::bad_alloc` is not swallowed. No path logs and continues with a partially transformed basis.

### Section 3: Security and Threat Model

| Threat | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Corrupt OOC lengths trigger overflow or huge allocation | Low | High | Checked multiplication, file-range validation, shared count caps, corruption tests |
| Crafted environment values enable unbounded weights/threads | Medium | Medium | Table-driven parsing and clamped numeric limits; invalid values fail visibly |
| Snapshot path collision or traversal | Low | High | Existing trusted base-path construction; no shell interpolation; explicit files only |
| Adversarial relation graph causes CPU/memory exhaustion | Medium | High | Weight/source/fill/batch caps, bounded candidate queue, peak metrics |
| Thread race publishes partial state | Low | High | Read-only snapshots, disjoint rows, ordered single-thread commit, TSAN |

No endpoint, credential, dependency, PII flow, or authorization surface is added. Auditability is provided by the stable generation, policy reason, reduction digest, limits, and stop reason.

### Section 4: Data Flow and Edge Cases

```text
RAW RELATIONS
  | nil/missing -> InvalidInput
  | empty       -> valid empty result, NoCandidates
  | corrupt     -> indexed validation error
  v
CANONICAL KEYS
  | even exponent -> key absent
  | high bits     -> preserved
  | allocation    -> propagate
  v
INCIDENCE + PURGE
  | singleton cascade -> fixed point
  | empty graph       -> valid full-only result
  | mismatch          -> abort transaction
  v
PLAN + PARALLEL MATERIALIZE
  | stale candidate -> discard/rescore
  | cap exceeded    -> reject without mutation
  | worker error    -> discard entire batch
  v
ORDERED COMMIT + VALIDATE
  | digest mismatch -> abort
  | no candidates   -> exact active basis
  | objective reached -> exact active basis
  v
PERSIST / MATRIX BUILD
  | round-trip mismatch -> test/abort
  | thin matrix         -> BW path
```

Boundary cases include 0/1/2/8/9 pivot weight, zero and maximum worker counts, duplicate source IDs, empty source XOR, exponent 0/1/254/255/256 totals, counts at and above persistence limits, roots and primes at `2^32 - 1`, `2^32`, and `UINT64_MAX`, exact packed-AB collisions, merged inputs with nested pairs, empty/full-only corpora, snapshot at zero rows, crash at each commit boundary, and finalize after a failed snapshot.

### Section 5: Code Quality Review

Accepted constraints:

- Put LP-key/parity helpers in a neutral relation header, not inside either merger.
- Keep one policy function and one engine implementation; test drivers call library APIs instead of copying pipeline blocks.
- Separate candidate selection, planning, materialization, and commit so no method owns more than one state transition.
- Use explicit result/stop/error enums. Avoid configuration booleans whose combinations create hidden modes.
- Do not add a generic graph framework. The bounded LP incidence graph is the only abstraction needed.
- Keep legacy V0/V3 untouched during M1 except for canonical helper adoption and targeted regression checks.

### Section 6: Test Review

```text
NEW DATA FLOWS
  canonical key extraction
    -> unit + property + MatrixBuilder agreement
  collector snapshot/append/finalize
    -> in-memory/OOC integration + corruption/resume
  shared route selection
    -> route equivalence + double-reduction rejection
  structured reduction
    -> exact GF(2) oracle + provenance + persistence round trip

NEW ASYNC PATHS
  incidence sharding
  candidate scoring
  conflict batching
  parallel materialization
    -> thread equivalence + exception join + TSAN

NEW POLICY PATHS
  unset / 0 / 1 / auto / invalid
  supported / unsupported size
  target / no-candidate / cap / invariant stop
    -> table-driven unit + pipeline integration

SCALE PATHS
  overlapping 5K / 50K / 200K hypergraphs
  17/27/40/81-bit gate
  real 100-150-bit sieve-to-reduction
  bounded 50-digit first round
```

The “2am Friday” test serializes a deep 8-way result, reads it through both stream and OOC formats, builds the matrix, and proves its rows map to the original source space. The hostile test uses high-bit key collisions, even exponents, stale generations, overlapping candidates, and a worker exception in one batch. The chaos test interrupts an OOC snapshot around flush/sentinel/seek boundaries and verifies either a usable append cursor or a visible ERROR state.

All randomized tests use a recorded seed and deterministic minimization output. New and modified relation tests use always-on checks in Release.

### Section 7: Performance Review

The three expected slow paths are:

1. Canonical incidence construction: `O(total LP entries)` plus deterministic sorting.
2. Candidate scoring: bounded by `max_column_weight^2` pair scores and source/row key XOR cost.
3. Materialization: proportional to selected source factors and LP entries; this is the primary parallel target.

Memory is the first scale constraint. Store immutable sources once, active combinations as sorted IDs, and one batch of materialized outputs. Tombstone obsolete rows and compact only at deterministic barriers. Do not cache all pair scores across generations.

The baseline report must separate scoring, materialization, commit, and validation time. A headline worker speedup that leaves total filter time unchanged does not satisfy promotion.

### Section 8: Observability and Debuggability

Every reduction emits one stable record with:

- input generation and canonical input digest;
- selected strategy and policy reason;
- rows, LP columns, excess, and nonzeros before/after;
- singleton, 2-way, higher-way, stale, fill, source, and persistence counters;
- batch count, worker count, scoring/materialization/commit/validation timings;
- output digest and stop/error code.

The digest is accepted as a small selective expansion because it proves route and thread equivalence without a full trace file. When an invariant fails, log the first row/pivot/source witness and keep the immutable input generation identifiable. No dashboard is required; bounded experiment reports are the operational artifact.

### Section 9: Deployment and Rollout

```text
M1 contracts -> OFF-mode equivalence -> M2 sequential oracle
     -> M3 thread equivalence -> M4 explicit flag integration
     -> M5 cross-size/OOC evidence -> M6 promotion decision
```

Rollback flow:

```text
structured failure or regression?
          |
          +-- before promotion --> unset/0 flag -> V0/V3 baseline
          |
          +-- after promotion  --> set flag 0 immediately
                                  |
                                  +--> revert default-policy commit
                                  +--> preserve diagnostic corpus
```

There is no database migration. Increasing a reader limit without changing the wire layout remains backward-compatible, but old binaries may reject newly deep rows. Therefore structured output is never written to a corpus intended for an older binary without recording the producing version and configured limit.

Post-integration verification runs OFF and ON modes on the same fixed corpus and compares source-space validity, then exercises one adaptive OOC snapshot cycle.

### Section 10: Long-Term Trajectory

Reversibility: 5/5 before default promotion and 4/5 afterward because persisted deep rows may exceed older reader limits.

Debt intentionally introduced:

- V0/V3 coexistence through the measurement window;
- in-process rather than standalone distributed filtering;
- fixed starting caps before measured auto-tuning.

The shared engine, canonical incidence, generation digest, and `RelationSource` boundary are platform capabilities. Future merge policies, yield experiments, or external reducers can reuse them without copying pipeline routing.

Deferred work is explicit: standalone distributed filter, 3LP/cofactor retuning, V0/V3 retirement, and policy auto-tuning after measured corpora.

### Section 11: Design and UX

Skipped after checking the plan for screens, components, interaction flows, responsive behavior, and design-system changes. This is a backend C++ pipeline change with no UI scope.

### NOT in Scope After Review

- Standalone/distributed filter executable: wait for in-process matrix gain.
- 3LP and cofactor-yield retuning: separate causal experiment.
- V0/V3 deletion: wait for compatibility and promotion evidence.
- Automatic cap tuning: requires multiple measured size bands.
- Performance UI/dashboard: stable logs and reports are sufficient.
- Global conversion of historical test assertions: convert only touched relation suites and prevent new Release-inactive checks.

### Dream State Delta

After this project, GNFS has one relation-reduction contract, exact source-space transformations, deterministic parallel execution, and bounded-memory integration. The remaining distance to the 12-month ideal is automatic policy learning from corpora, standalone distributed reduction when scale proves it necessary, and retirement of legacy strategies.

### Failure Modes Registry

| Codepath | Failure mode | Rescued? | Test? | Visible? | Logged? |
|---|---|---:|---:|---:|---:|
| canonical LP helper | exponent/key disagreement | Yes, reject | Yes | Yes | Yes |
| snapshot | I/O or invalid state | Yes, restore/error state | Yes | Yes | Yes |
| reduction routing | duplicate or skipped generation | Yes, reject | Yes | Yes | Yes |
| incidence | synchronized views diverge | Yes, abort transaction | Yes | Yes | Yes |
| planner | no admissible candidate | Normal stop | Yes | Yes | Yes |
| materializer | source/exponent/persistence cap | Yes, reject/abort | Yes | Yes | Yes |
| worker batch | exception or overlap | Yes, discard batch | Yes | Yes | Yes |
| validator | pivot/dependency-map/digest mismatch | Yes, fail closed | Yes | Yes | Yes |
| MatrixBuilder handoff | persistence or parity drift | Yes, abort | Yes | Yes | Yes |
| thin solve handoff | early return bypass | Yes, route fixed | Yes | Yes | Yes |

There are no planned silent, untested, unlogged failures.

### Stale Diagram Audit

The plan touches lifecycle diagrams embedded as comments in `collector.hpp` and `ooc_relation_store.hpp`; their “write then finalize then read” descriptions become stale when snapshot semantics land and must change in M1. Pipeline filtering comments in `pipeline.cpp` also become stale when routing is centralized. No user-facing design diagrams are involved.

### CEO Implementation Tasks

- [x] **T1 (P1, human: ~4h / agent: ~45m)** — LP contract — implement the full-width exponent-parity helper and MatrixBuilder agreement tests.
- [x] **T2 (P1, human: ~1d / agent: ~2h)** — OOC lifecycle — implement snapshot/finalize states with append and corruption tests.
- [x] **T3 (P1, human: ~1d / agent: ~2h)** — routing — centralize reduction and prove OFF-mode route equivalence.
- [x] **T4 (P1, human: ~2h / agent: ~30m)** — solver handoff — make thin solving reachable and add a regression.
- [ ] **T5 (P1, human: ~3d / agent: ~6h)** — structured core — M2a
  2-way purge, M2b weight-[3,8] star/MST plan/prepare/commit, and M2c budgeted
  sequential orchestration are complete with exact projection and
  dependency-kernel oracles; the full `MatrixBuilder` payload oracle remains.
- [ ] **T6 (P1, human: ~2d / agent: ~4h)** — persistence — normalize materialization and prove stream/OOC/MatrixBuilder round trips.
- [ ] **T7 (P2, human: ~2d / agent: ~4h)** — parallel scheduler — add conflict batches, ordered commit, thread equivalence, and TSAN.
- [ ] **T8 (P2, human: ~2d / agent: ~4h)** — integration evidence — policy, documentation, realistic scale corpora, and cross-size validation.

### CEO Review Completion Summary

```text
+====================================================================+
| Mode selected        | SELECTIVE EXPANSION                          |
| System audit         | 6 blocking contract groups; dirty .gitignore |
| Step 0               | Approach B; 2 of 6 expansions accepted       |
| Section 1  (Arch)    | 3 issues resolved in the plan                |
| Section 2  (Errors)  | 10 paths mapped, 0 critical gaps             |
| Section 3  (Security)| 5 threats, 0 unmitigated High threats        |
| Section 4  (Data/UX) | shadow paths and boundaries mapped           |
| Section 5  (Quality) | 6 constraints frozen                         |
| Section 6  (Tests)   | full diagram; hostile and chaos cases added  |
| Section 7  (Perf)    | 3 slow paths; memory is first constraint     |
| Section 8  (Observ)  | stable digest and timings accepted           |
| Section 9  (Deploy)  | flag rollback and version caveat documented  |
| Section 10 (Future)  | reversibility 5/5 pre-promotion              |
| Section 11 (Design)  | skipped, no UI scope                         |
+--------------------------------------------------------------------+
| NOT in scope         | 6 explicit items                             |
| What already exists  | 9 components mapped                         |
| Dream state delta    | written                                      |
| Error/rescue registry| 10 paths, 0 critical gaps                    |
| Failure modes        | 10 rows, 0 critical gaps                     |
| Scope proposals      | 6 proposed, 2 accepted, 3 deferred           |
| Lake score           | 6/6 completeness decisions                   |
| Diagrams produced    | architecture, state, data, deploy, rollback  |
| Unresolved decisions | 0                                            |
+====================================================================+
```

## Engineering Review Record

### Scope Challenge and Actual-Code Findings

The review read the production call paths, storage format, checkpoint contract, MatrixBuilder LP construction, V0/V3 classification, algebraic-square-root validation, and current tests. The project remains warranted, but implementation is gated by the following consolidated findings; every P1 is folded into M0/M1 rather than left as an implementation-time interpretation.

| # | Priority | Finding | Evidence | Resolution in this plan | Confidence |
|---:|---|---|---|---|---:|
| 1 | P1 | Correctness target was row-space equality instead of dependency-space isomorphism | A weight-`k` column replacement intentionally changes row space | Freeze `M`, `T`, `F` and the `ker(F^T) --T^T--> ker(M^T)` bijection; exact oracle covers all matrix payload columns | 10/10 |
| 2 | P1 | Raw `(a,b)` packing collides and structured primary-pair dedup drops valid MST outputs | `pipeline.cpp:1055-1063,1339-1349`; `distributed_sieve.cpp:548`; signed shift is non-portable | Full `ABPair` raw identity before source IDs; source-combination identity afterward; collision regressions | 10/10 |
| 3 | P1 | LP parity/width/classification drift spans filters, V0/V3, matrix, metrics, and sqrt | `filter.hpp:115-205,227-278,299-460,545-805`; `matrix_builder.hpp:561-599,926-990`; `algebraic_sqrt.hpp:41-65` | One neutral full-width odd-key visitor and complete consumer sweep in M1a | 10/10 |
| 4 | P1 | Materialization loses nested `extra_ab_pairs` | Public filter accepts already-merged relations; first-source-only wording discarded inner provenance | Flatten primary plus every nested pair in stable source order; matrix/sqrt/round-trip tests | 9/10 |
| 5 | P1 | Vector-only results contradict bounded-memory OOC promotion | `RelationReductionResult`, `Pipeline`, and `MatrixResult` currently retain vectors | Own `InMemoryCorpus` or `OwnedOOCCorpus`; source/sink reducer; direct matrix and selected-dependency reads | 10/10 |
| 6 | P1 | OOC snapshot/resume has no committed prefix and may silently count failed writes | `collector.hpp:249-259`; `ooc_relation_store.hpp:124-169`; checkpoint lacks relation descriptor | Explicit writer states, trusted committed-prefix descriptor, versioned paired checkpoint, child-process failpoints | 10/10 |
| 7 | P1 | Thin Block Wiedemann exists but an earlier pipeline gate bypasses it | `pipeline.cpp:2183-2193` versus thin path in `solve_matrix()` | Make solver the final authority; isolate this expected behavior change in M1d | 10/10 |
| 8 | P1 | V3 residual accounting can emit a later unmerged singleton | `clique_merger.hpp:210,292` reuses global `visited.size()` in a per-start decision | Add source-accounting regression and fix in M1a compatibility sweep | 9/10 |
| 9 | P2 | Fill scoring and stop semantics were underspecified | Pair scoring proposed materialization; `FillBudgetReached` had no budget; invariant was a normal stop | Metadata-only scoring, frozen total comparator, LP/full-matrix budgets, invariant-only error channel | 10/10 |
| 10 | P2 | Existing scale fixtures cannot support promotion claims | `test_clique_merger_50d_synthetic.cpp:70-126` lacks real overlap; touched tests use bare `assert()` | Fixed overlapping corpora, always-on checks, frozen 120-bit/50-digit parameters and materiality thresholds | 10/10 |

The scope is accepted at full size. Reducing it to a V3 patch would leave duplicate routing, undefined OOC append behavior, and no trustworthy scale conclusion. New infrastructure is limited to one corpus boundary, one reducer, one structured strategy, and the existing thread pool.

### What Already Exists

| Capability | Existing implementation | Reuse decision |
|---|---|---|
| End-to-end GNFS pipeline and thin solver | `src/api/pipeline.cpp` | Reuse; remove the earlier terminal gate and centralize relation routing |
| Full-width matrix LP semantics | `include/gnfs/linalg/matrix_builder.hpp` | Use as behavioral baseline, then make both collection/build paths consume the canonical helper |
| V0/V3 mergers | `filter.hpp`, `clique_merger.hpp` | Retain as opt-in baselines/fallbacks through M6; repair contract consumers and source-accounting bug |
| Relation/OOC serialization | core relation streams and `ooc_relation_store.hpp` | Version and extend one format; do not create a second incompatible store |
| Collector and sieve checkpoint | `collector.hpp`, `sieve_checkpoint.hpp` | Add an explicit paired committed-prefix state machine |
| Parallel runtime | `gnfs::util::ThreadPool` | Reuse with immutable scoring snapshots, conflict batches, and ordered commit |
| Sparse GF(2) operations | linalg SGE/row operations | Reuse only as an independent small-case oracle |
| Test runner and catalog | `scripts/test.sh`, CMake | Register all binaries and derive live counts/times from the catalog |

### Architecture Review

```text
special-Q/distributed producers
           |
           v
  RelationCollector (Open)
           |
           +-- checkpoint_prefix --> OOCSnapshotDescriptor
           |                            |
           |                            +--> SieveCheckpoint V2 commit
           +-- snapshot/finalize ------+
                                        v
                         move-only RawRelationSnapshot
                                        |
                                        v
                         RelationReductionEngine
                       / legacy | structured \
                      /         |            \
             canonical LP   dependency T   ordered batches
                      \         |            /
                       \        v           /
                         RelationSink -> RelationCorpus owner
                                        |
                         +--------------+--------------+
                         v                             v
                  MatrixBuilder source        selected dependency reads
                         |                             |
                         +----------> solver ----------+--> sqrt
```

Coupling is contained by the neutral key helper and corpus source/sink interface. Scaling pressure appears first in source-set/incidence memory, so output-vector materialization cannot be the promoted design. The only security-sensitive boundary is untrusted/corrupt persisted lengths and offsets; readers use checked arithmetic and fail closed. Inline state diagrams should be added to `ooc_relation_store.hpp`, `collector.hpp`, and the new reduction engine because their transitions are non-obvious.

### Code Quality Review

1. Remove production copies of packed AB identity rather than wrapping them; one `ABPairHash` is simpler and correct.
2. Keep raw-storage queries (`is_full`, `num_large_primes`) distinct from GF(2) canonical queries (`has_odd_large_prime_keys`, count/visitor), with comments preventing semantic substitution.
3. Keep candidate planning, metadata scoring, materialization, and commit as separate functions; no generic graph/task framework is warranted.
4. Use move-only snapshot and corpus-owner types instead of `reduced_once` booleans or path-lifetime conventions.
5. Use one normal stop enum beginning at `NotStarted`; invariant violations use contextual errors and never publish partial results.

### Test Coverage Diagram

```text
CANONICAL CONTRACT
  odd LP helper
    +-- [ADD UNIT/PROPERTY] e parity, side, p/r high bits, XOR homomorphism
    +-- [ADD AGREEMENT] MatrixBuilder collect/build, V0/V3, separation, sqrt
  identity
    +-- [ADD REGRESSION] packed AB collision
    +-- [ADD REGRESSION] shared-primary structured outputs survive

PERSISTENCE AND RECOVERY
  writer states
    +-- [ADD INTEGRATION] snapshot -> append -> snapshot -> finalize
    +-- [ADD FAILURE] invalid state and I/O failure preserve committed count
  paired checkpoint
    +-- [REPLACE FALSE RESUME] child `_Exit()` at every commit boundary
    +-- [ADD CORRUPTION] truncated/non-monotonic/overflow/wrong generation
  corpus owner
    +-- [ADD INTEGRATION] vector/OOC matrix and selected dependency lifetime

REDUCTION ROUTING
  raw snapshot -> engine -> corpus
    +-- [ADD ROUTE] adaptive/distributed/public/stress/progressive/final digest
    +-- [ADD TYPE/REGRESSION] each generation consumed once
    +-- [ADD HANDOFF] thin solver reached exactly once

STRUCTURED MATHEMATICS
  singleton -> 2-way -> k-way MST
    +-- [ADD EXACT ORACLE] dependency dimension and `T^T` bijection
    +-- [ADD PAYLOAD] sign, FB, QC, Schirokauer, LP columns
    +-- [ADD PROVENANCE] nested merge parenthesization and sqrt reconstruction
    +-- [ADD PARALLEL] 1/2/4/hardware rows, order, stats, digest, stop

SCALE AND RELEASE
  fixed overlap corpus -> 120-bit path -> bounded 50-digit round
    +-- [ADD SCALE] histogram, dependency dimension, full NNZ, RSS, timings
    +-- [EXISTING GATE] 17/27/40/81-bit OFF-mode factorization
```

All new paths currently require tests because implementation has not begun. The gaps are accepted into M1-M5, not deferred. An external gstack QA artifact records the branch-specific test plan.

### Performance Review

1. Scoring every pair by fully materializing relations would multiply allocations and integer copies. Score canonical key/source/factor metadata and materialize only selected MST edges.
2. Repeated full accumulated OOC snapshots are O(rounds × relations) I/O and memory. The appendable snapshot fixes lifecycle correctness; promoted scale requires direct source/sink reduction rather than repeated vectors.
3. Ordered commit can dominate at scale. Measure batch occupancy and commit share before considering concurrent incidence mutation; redesign batch size if commit exceeds the frozen budget.
4. Full-matrix fill, not LP weight alone, controls solver cost. Record both projected and realized matrix nonzeros and reject pivots under a global budget.

### Engineering Failure Registry

| Codepath | Production failure | Test planned | Handling planned | Silent after implementation? |
|---|---|---:|---:|---:|
| canonical key | high-bit or exponent cancellation drift | Yes | Contract helper and consumer agreement | No |
| raw identity | packed collision drops a relation | Yes | Full `ABPair` identity | No |
| materializer | nested pair or factor provenance lost | Yes | Immutable-source reconstruction and exact checks | No |
| writer/checkpoint | SQ progress outruns committed relation prefix | Yes | Paired versioned descriptor; fail closed | No |
| prefix reader | corrupt size/offset causes OOB/allocation | Yes | Checked arithmetic and runtime validation | No |
| corpus owner | files/vectors die before matrix/sqrt use | Yes | Owning result lifetime | No |
| engine route | generation reduced zero or twice | Yes | Move-only input and route digest | No |
| structured pivot | dependency nullity changes | Yes | Exact oracle and pre-publish validation | No |
| worker batch | exception or overlap partially commits | Yes | Join/discard batch; ordered commit | No |
| scale policy | nominal excess hides matrix regression | Yes | Frozen materiality and full-NNZ budgets | No |

Critical gaps after plan revision: 0. Before the listed tests and handlers land, structured mode remains unavailable and legacy default behavior remains unchanged.

### NOT in Scope After Engineering Review

- Standalone distributed reducer and segment manifest: defer until in-process matrix gain and corpus format stabilize.
- 3LP/cofactor-yield retuning: keep as an independent causal experiment.
- V0/V3 removal: retain baselines and rollback through the promotion window.
- Generic graph framework or new task runtime: the bounded incidence model and existing thread pool suffice.
- Automatic cap/policy learning: needs reproducible multi-size reports.
- Whole-repository conversion from `assert()`: convert touched relation/checkpoint suites and forbid new inactive checks.

These deferred items are recorded in `TODOS.md` with rationale and dependencies.

### Retrospective and Outside Voice

Recent branch ancestry contains the harness-engineering series that centralized project contracts, test catalog checks, local-path scanning, and pull-request workflow. This plan follows that precedent: volatile counts stay in `scripts/test.sh`, contracts live in one public API, and generated/local state is excluded from commits. No prior structured-filter implementation exists on this branch to preserve.

An independent adversarial plan reviewer reported `DONE_WITH_CONCERNS`. Its six P1 groups—dependency proof, dedup identity, full-width sqrt coverage, nested provenance, corpus ownership, and crash-safe OOC state—are all folded into M0/M1. Separate LP-contract and OOC-lifecycle reviewers independently confirmed the consumer drift and missing committed prefix. No outside-voice objection remains unresolved.

### Engineering Review Completion Summary

```text
Step 0: Scope Challenge       — scope accepted at full size after six P1 contract corrections
Architecture Review           — 6 blocking boundaries found and resolved in the plan
Code Quality Review           — 5 issues found; explicit minimal abstractions selected
Test Review                   — diagram produced; all new-path gaps assigned to M1-M5
Performance Review            — 4 issues found; scoring, OOC, commit, and fill budgets frozen
NOT in scope                  — written
What already exists           — written
TODOS.md updates              — 4 deferred items recorded
Failure modes                 — 0 critical gaps after planned coverage/handling
Outside voice                 — issues found and folded; 0 unresolved
Lake Score                    — 10/10 consolidated findings chose the complete option
Unresolved decisions          — 0
```

<!-- AUTONOMOUS DECISION LOG -->
## Decision Audit Trail

| # | Phase | Decision | Principle | Rationale | Rejected |
|---:|---|---|---|---|---|
| 1 | CEO | Keep full structured-reduction scope, opt-in first | Correctness and reversibility | A V3-only patch cannot produce trustworthy scale evidence | Patch-only approach |
| 2 | CEO | Accept corpus source/sink boundary | Completeness | Required for bounded-memory OOC promotion and lifetime safety | Vector-only result |
| 3 | CEO | Accept stable generation digest | Reproducibility | Makes route/thread equivalence observable | Logs without replay identity |
| 4 | CEO | Defer standalone distributed reducer | Minimal proven infrastructure | Algorithm gain must precede a second execution stack | Build now |
| 5 | CEO | Defer 3LP/yield tuning | Causal evidence | Avoid mixing collection yield with reduction quality | Bundle experiments |
| 6 | Eng | Prove dependency-space bijection | Mathematical correctness | Row space intentionally changes during elimination | Row-space equality |
| 7 | Eng | Split raw and structured identity | Exact provenance | Primary pairs are not unique structured rows | Packed/primary dedup |
| 8 | Eng | Sweep every full-width LP consumer | One source of truth | Matrix-only agreement would leave sqrt/classification wrong | Partial helper adoption |
| 9 | Eng | Flatten nested merged provenance | End-to-end correctness | Public inputs may already contain `extra_ab_pairs` | Raw-only silent assumption |
| 10 | Eng | Pair relation prefix with SQ checkpoint | Crash consistency | Resume must never skip work beyond durable relations | Flush-only best effort |
| 11 | Eng | Use metadata-only scoring and global fill budgets | Measured performance | Full materialization is wasteful and LP weight is incomplete | Pair materialization |
| 12 | Eng | Separate thin-solver fix from OFF equivalence | Honest validation | The handoff intentionally changes end-to-end behavior | One ambiguous byte-equality gate |
| 13 | Eng | Forced invariant errors fail explicitly | Fail closed | Silent legacy fallback would mask corruption | Automatic fallback |
| 14 | Eng | Preserve user `.gitignore` change | Repository hygiene | It is unrelated user-owned work | Include or discard it |

## GSTACK REVIEW REPORT

```text
Plan                       docs/plans/2026-07-21-parallel-structured-filter.md
CEO review                 CLEAN — selective expansion, 0 unresolved decisions
Design review              SKIPPED — no UI scope
Engineering review         CLEAN — 10 consolidated findings folded into plan
Independent outside voice  DONE_WITH_CONCERNS -> all concerns resolved in plan
Test plan artifact         recorded in the external gstack project directory
Deferred-work artifact     TODOS.md
Critical gaps              0 after required M1-M5 coverage and handling
Implementation gate        M0 contract/baseline freeze, then M1a-M1d
```

NO UNRESOLVED DECISIONS
