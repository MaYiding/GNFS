# GNFS Development Roadmap

This roadmap orders follow-up work by mathematical correctness, portability,
measured performance, and maintenance cost. It describes acceptance criteria,
not completed work. Query the current test catalog and pull requests before
starting a task; historical timings and CI results do not establish a baseline
for a new commit.

## Immediate Correctness Work

### Parallel Task Lifetimes

Every synchronous parallel helper must finish all accepted tasks before it
returns or throws. Cover both callback failure and failure while submitting a
later task. A partially populated future list must keep the callback, work
cursor, and borrowed input alive until its workers finish.

Start with `ThreadPool::parallel_for`, `parallel_for_index`, and
`parallel_for_stealing`. Add deterministic failure injection after at least one
task has started, check the original exception, and prove the pool remains
usable. Run Release-active regressions and the relevant sanitizer lane. Keep
submission failure separate from algorithm cancellation and deadline policy.

### Arithmetic Domains and Aliasing

Public `Integer` helpers should reject unsupported domains before GMP can
terminate the process. Audit square root, modular exponentiation, inversion,
and division together with their callers. Preserve valid negative-exponent
and negative-modulus behavior; document the exception contract.

Audit mutating polynomial operations for operands that refer to coefficients
or to the polynomial itself. Compare results with independent exact-integer
identities, including zero, negative values, and multi-limb coefficients.
Preserve normalization and avoid changing the mathematical representation.

### Recovery Data

Extend checkpoint validation from wire shape and checksums to the exact
computation identity and reachable algorithm state. A valid checksum does not
prove that a cursor, matrix, polynomial, or factor base belongs to the current
run. Reject incompatible state before mutating live objects, then verify
fresh-run parity. Exercise allocation failures without allocating an enormous
real payload or suppressing sanitizer coverage for the ordinary parser.

## Reproducible Cross-Platform Builds

### Windows Bootstrap and Runtime Closure

The Windows release path pins compiler/runtime archives, but build tools can
still introduce a newer runtime through the live MSYS2 package repository.
Package splits can make a later downgrade invalid even when every pinned
archive digest is correct.

Choose and document one coherent closure:

- Freeze the bootstrap tools and all transitive dependencies needed by the
  existing compiler/runtime set.
- Migrate the compiler, split runtime packages, bootstrap tools, source
  archives, licenses, and validation manifests together.

Keep dependency resolution enabled. Do not use forced installation, remove
runtime packages still owned by build tools, or accept whichever DLL happens
to be present. Record package identity, archive digest, source identity, and
the reason each package belongs to the closure. Preserve both subprocess
output streams so installation failures remain diagnosable.

Acceptance requires a clean Windows worker, installation of the selected
closure, process-tree supervisor execution, build/install, exact DLL and
source verification, and package smoke tests. macOS script self-tests cannot
substitute for that Windows evidence. See [Releasing](../releasing.md).

### Test Runner Selection

Keep selection separate from execution: an explicitly requested test must
either run or fail visibly. Validate target names before configuration, use
CMake's generator-independent options, and normalize build paths before
passing executable paths to the supervisor.

Extend targeted builds to module and changed-file modes only after recording
their selected executable and helper dependencies. Preserve CTest-only
contracts and non-binary tests. Compare fresh and incremental builds using
the same generator, compiler, build type, and selected tests; do not infer a
speedup solely from removing work that the requested mode still requires.

## Measured Algorithm Scaling

Use the [SIQS matrix scaling design](../perf/siqs-shadow-matrix-scaling.md) and
[structured out-of-core measurements](../perf/structured-ooc-measurement.md)
to choose the next experiment. Keep production promotion separate from
shadow correctness and telemetry changes.

For sparse matrix, relation filtering, and large-prime policies, record actual
column counts, independent dependency rank, peak resident memory, and wall
time. Validate the small gate including the 81-bit case, a relevant
100-150-bit path, and a bounded 50-digit experiment. Preserve exact large-prime
column accounting rather than substituting ratios measured on smaller inputs.

Compare worker counts with deterministic inputs and record compiler, input,
seed, runtime flags, and build identity. A performance claim needs comparable
before/after measurements plus correctness parity. Do not tune multiple
unrelated thresholds in the same experiment.

## Delivery and Artifact Lifecycle

Keep each fix independently reviewable, with a failing-before reproduction
and the narrowest meaningful passing-after validation. Run broader checks
when the changed dependency surface warrants them. The
[testing policy](../testing-ci-policy.md) remains authoritative for selection.

Publish focused branches through pull requests. Continue development while
CI runs, and inspect failures against the exact commit and job. Distinguish
inherited toolchain failures from new regressions. Respect repository review
and protection rules when merging; an open PR is not a completed integration.

Retain active builds and failed-run evidence until diagnosis is complete.
Remove obsolete generated directories only after checking ownership, active
processes, and whether their reports are the only evidence for a result.
Prune stale worktree registrations separately from source branches, and
preserve untracked source files and recovery references.
