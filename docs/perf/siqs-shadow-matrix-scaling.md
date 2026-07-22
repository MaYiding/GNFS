# SIQS Shadow Matrix Scaling

## Scope

This document records the scaling evidence and promotion gates for the staged
Self-Initializing Quadratic Sieve (SIQS) shadow matrix. It covers the
deterministic dense solver in
`include/gnfs/siqs/shadow_matrix.hpp`. It does not claim production
two-large-prime (2LP) yield or authorize connecting the shadow path to
`factor()`.

`include/gnfs/siqs/shadow_proof_runner.hpp` adds a production-facing but still
unwired read-only facade. Its default admission envelope is 32768 raw
relations, 64MiB of portable logical payload, 16384 graph edges, 4096 cycles,
262144 total cycle incidences, 4096 row candidates, and 4096 pretrim rows. The
facade also preserves the solver's independent dense byte and variable limits.
All limits are inclusive; the next object returns a typed fallback instead of
allocating beyond the admitted envelope.

The facade keeps the caller's raw relations intact so a later integration can
fall back to the current merge and solver. Consequently, its production peak
will include raw storage, owning assembly rows, and the packed matrix at the
same time. The 256-A proof executable releases raw storage before solving, so
its measured peak is correctness evidence, not a production RSS projection.

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
  dependency, and factor boundaries; not yet called by `factor()`.
- [ ] Controlled collector and `factor()` integration with the default 1LP path
  unchanged.
