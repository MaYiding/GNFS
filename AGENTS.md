# GNFS Agent Guide

This file is the shared, tool-agnostic operating guide for coding agents working on GNFS. Keep it concise and current. Put detailed design notes in `docs/`, repeatable procedures in project skills, and mechanically enforceable checks in Hooks or CI.

## Mission and Priorities

GNFS is a C++20 implementation of the General Number Field Sieve. Optimize in this order:

1. Mathematical correctness and reproducibility.
2. Cross-platform correctness on macOS, Linux, and Windows.
3. Measured performance on representative input sizes.
4. Maintainability and clear validation evidence.

Follow the user's current request over repository defaults. Preserve unrelated user changes and avoid expanding scope without evidence.

## Sources of Truth

- Build and test entry point: `scripts/test.sh`.
- Current test catalog, tiers, and timeouts: `./scripts/test.sh list`.
- CI selection policy: `docs/testing-ci-policy.md`.
- Runtime tuning flags: `docs/env-flags/README.md` and its module pages.
- Formal writing rules: `docs/writing-style-guide.md`.
- Claude Code integration: `.claude/settings.json`, `.claude/agents/`, and `.claude/skills/`.

Do not copy volatile test counts, timings, file counts, or flag inventories into agent instructions. Query the live sources instead.

## Working Loop

1. Inspect `git status --short --branch` and the relevant code, tests, and docs.
2. Reproduce a bug or establish a baseline before changing behavior.
3. Make the smallest coherent change that solves the task.
4. Run the narrowest meaningful validation, then widen it when risk warrants.
5. Review the final diff for unrelated edits, generated files, secrets, local paths, and missing docs.
6. Report what changed, exact validation commands, and any remaining risk.

Use a written plan for multi-file or high-risk work. Do not force plans, subagents, debuggers, or persistent tracking files onto routine changes.

## Build and Test

Prefer the project runner because it owns configuration, build parallelism, timeouts, and reporting:

```bash
./scripts/test.sh build
./scripts/test.sh smoke
./scripts/test.sh changed
./scripts/test.sh changed --deep
./scripts/test.sh module linalg
./scripts/test.sh run test_linalg
./scripts/test.sh gate
./scripts/test.sh e2e
./scripts/test.sh list
```

Validation should match risk:

| Change | Minimum validation |
|---|---|
| Docs or Harness only | Dedicated checker or syntax test |
| Local helper or unit behavior | `./scripts/test.sh changed` or one module |
| Shared core or dependency-facing change | `./scripts/test.sh changed --deep` |
| Pipeline, relation filtering, matrix, or square root | Relevant module plus `gate` or `e2e` |
| Size-sensitive algorithm or performance policy | Cross-size tests described below |

Do not run `full`, `thorough`, `nightly`, benchmark, or stress modes by habit. Use them when the change or user request justifies their cost. Long-running commands need visible progress and a bounded timeout or a documented background-run handoff.

## Architecture Map

The pipeline is polynomial selection -> factor base -> lattice sieve -> cofactorization -> relation filtering and merging -> GF(2) linear algebra -> algebraic and rational square root -> GCD.

Code lives under `include/gnfs/<module>/` and `src/<module>/`; tests live under `tests/`. The main modules are `api`, `core`, `polynomial`, `factor_base`, `sieve`, `cofactor`, `relation`, `linalg`, `sqrt`, `siqs`, and `util`.

## Non-Negotiable Correctness Invariants

- Algebraic elements use `a - b*alpha`, never `a + b*alpha`.
- A GF(2) matrix may use only `schirokauer_primes = {2}`. Maps for primes greater than 2 require matrix entries modulo that prime.
- Reject relations with `gcd(a - b*m, N) > 1`; they create degenerate dependencies.
- Use `gnfs::core::Integer` for large integers and GMP-backed arithmetic.
- Relation trimming with large-prime columns must use:

  ```cpp
  const size_t lp_cols = lp_enabled ? count_unique_lp_keys(relations) : 0;
  const size_t effective_cols = matrix_cols + lp_cols;
  const size_t max_rels = static_cast<size_t>(effective_cols * safety_factor);
  ```

  Never size the trim limit from bare factor-base columns when large primes are enabled.

## Size-Sensitive Changes

An 81-bit pass does not predict 50-digit or 60-digit behavior. Large-prime key distributions change sharply with `lp_bits`, so filter, merge, sieve-stop, relation-trim, and matrix-excess changes need multiple size bands.

For such changes, validate at least:

- the small gate band (including the 81-bit case);
- a 100-150-bit path such as `test_kleinjung_large` when relevant;
- a bounded 50-digit experiment or first round when the change can affect large-prime behavior.

Prefer exact quantities such as `count_unique_lp_keys()` over historical ratios or fixed percentages. Label any remaining heuristic with its measured size range.

## C++ and Portability Rules

- Use C++20, RAII, `std::optional`, `std::span`, concepts, and explicit ownership.
- Use `snake_case` for functions and variables and `PascalCase` for types.
- Add every standard-library header that a file directly relies on; do not depend on transitive libc++ includes.
- Account for LP64 differences: `int64_t` may alias `long` on Linux and `long long` on macOS.
- Clamp or range-check floating-point values before integer casts; out-of-range casts are undefined behavior.
- Do not use `assert()` as the only test expectation because `NDEBUG` removes it in Release builds.
- Avoid allocations and unnecessary `Integer` copies in hot loops, but require benchmark evidence before introducing specialized complexity.

## Tests and CMake

New tests require all of the following:

- a CMake test with `LABELS` and `TIMEOUT`;
- an entry in `scripts/test.sh` `ALL_TEST_BINARIES`, `TEST_TIMEOUT`, and `TEST_TIER` when the test has a binary;
- placement in `SMOKE_TESTS` only for deterministic `instant` tests;
- module mapping and slow-path mapping where applicable.

Run `./scripts/test.sh list` after catalog changes. Follow `docs/testing-ci-policy.md` for tier definitions; do not classify from Release timing alone.

## Documentation Placement

Read `docs/writing-style-guide.md` before formal README, design, commit-body, or PR-description work.

- Algorithm and mathematical derivations: `docs/algorithms/`.
- Performance designs and experiments: `docs/perf/`.
- Runtime flag details: the matching `docs/env-flags/<module>.md` page.
- Test timing and CI policy: `docs/testing-ci-policy.md`.
- Harness architecture and Hook contracts: `docs/harness-engineering.md`.

For a new `GNFS_*` flag, document its behavior, parsing, default, bit-for-bit contract, integration point, and tests on the module page; then update the env-flags index. Keep detailed flag design out of this file.

## Repository Hygiene and Git Safety

- Never edit generated output under root `build/`, `build-*`, `cmake-build-*`, `xcode-build/`, or any `CMakeFiles/` directory.
- Never commit machine-specific absolute paths, usernames, hostnames, tokens, credentials, private URLs, or local settings.
- Keep `.claude/settings.local.json` and `CLAUDE.local.md` untracked.
- Use relative paths, `${CLAUDE_PROJECT_DIR}`, `${CMAKE_SOURCE_DIR}`, `${PROJECT_SOURCE_DIR}`, or documented placeholders.
- Keep commits focused and independently understandable. Separate unrelated bug fixes.
- Do not rewrite history, force-push, merge, or push unless the user explicitly requests it.
- Do not discard or overwrite changes you did not create.

Before delivery, run the relevant secret/path scan or `python3 scripts/check_harness.py` for Harness changes, inspect `git diff --check`, and review the final status.

## Claude Code Harness

`CLAUDE.md` imports this file instead of duplicating it. Project Hooks enforce only cheap, deterministic boundaries; they must not launch builds, broad test suites, network calls, or model-based reviews. The committed settings contain team-shared Hooks only. Personal permissions belong in `.claude/settings.local.json`.

Use the project skills for explicit build/test and status workflows. Specialized agents are read-only reviewers or debuggers; invoking them is optional and should be driven by task complexity, not file count.
