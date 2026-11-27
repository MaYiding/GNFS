# Workbench Telemetry Contract

GNFS Workbench distinguishes a requested route from the algorithms that actually execute. Selecting GNFS does not imply that polynomial selection, sieving, or linear algebra ran: primality, perfect powers, trial division, Pollard Rho, ECM, or SIQS can finish first.

## Algorithms and Splits

The schema-1 event stream retains its existing fields and adds:

| Field | Meaning |
|---|---|
| `algorithm` | Algorithm executing at this progress event. |
| `current_number` | Composite remainder being processed. |
| `attempt_index` | One-based composite split number in complete mode; zero outside a numbered split. |
| `effective_parameters` | Resolved GNFS values, or `null` before GNFS executes. |

`started.method` describes the initial route selection. `stats.executed_algorithm` describes the algorithm that produced a split, or the last attempted algorithm on failure. Tags are `primality`, `perfect_power`, `trial`, `rho`, `ecm`, `siqs`, and `gnfs`. The complete result uses `mixed` when different splits finish with different algorithms. `algorithms_executed` lists unique algorithms in first-observed order, including unsuccessful probes.

The bounded event reader accepts individual lines up to 16MiB because complete results can include many split records (for example, `2^1024` creates 1,023 splits). Oversized lines still fail explicitly.

`result.attempts` contains each composite split, with its input, factors, algorithm, parameters, counters, and timings. These split results are not themselves complete prime factorizations. Complete-mode primality checks that require no split do not create attempts. A prime input therefore has no attempts and reports `executed_algorithm: "primality"`; its compatibility `method` is `auto`.

The compatibility `method` field keeps the existing enum. Complete mode chooses the most advanced executed family across splits. Perfect-power detection uses the trial family, and ECM uses the SIQS family. Consumers needing the exact algorithm must use `executed_algorithm`.

## Requested and Effective Parameters

The parameter sheet edits requested overrides transactionally. Blank input requests automatic values. The calculation panel reads effective values from telemetry; it never substitutes the requested values for measurements.

`gnfs_parameters_applied` indicates whether GNFS executed. When true, degree, factor-base bounds, large-prime bound, sieve width and height, and the special-q budget describe the resolved GNFS configuration. Polynomial degree follows the selected or resumed polynomial. Width counts the inclusive sieve interval, including odd-width overrides.

A complete result retains the last executed GNFS split's parameter fields for compatibility. Workbench displays effective parameters within individual splits rather than representing those fields as one configuration for a mixed task. Old history without this telemetry displays unavailable metadata explicitly.

Workbench resolves the engine executable before removing inherited `GNFS_*` variables from the child environment. It then injects only the managed `GNFS_RESUME` path. This prevents shell tuning flags from silently overriding a GUI run's configuration. Non-GNFS environment variables remain available.

## Counters and Time

- Progress counters are phase-scoped. Workbench updates relation counters during sieving and matrix dimensions during linear algebra. Zero placeholders from later phases do not erase previously measured values. Terminal result counters are authoritative.
- Each new split resets live counters, parameters, and rate samples. A return from filtering to sieving resets the rate baseline but retains earlier samples. Rate uses differences between sieve observations; preprocessing and filtering time do not dilute it.
- The sieve percentage is the relation count divided by the current relation target. It is not whole-task completion or an estimate of remaining time. Collection can stop below its target, and adaptive filtering can change the target.
- Relation reduction inside collection records filter time and the latest reduction counters. Successive snapshots overlap, so their row counts are not summed. Filter time accumulates across reductions and is excluded from sieve time.
- `partial_2lp` is the existing highest bucket: it includes relations with two or more persisted large-prime entries. Distributed relation classification uses the same buckets as the local collector.
- `candidates_statistics_complete: false` means the candidate count is unavailable, including distributed workers that do not report it. The legacy numeric field remains present for schema compatibility.
- SIQS statistics describe retained relations after merging and trimming, processed polynomials, and resolved sieve workers. They are separate from GNFS relations. If SIQS returns without statistics, the new SIQS fields are `null`, not measured zeroes.
- `non_gnfs_s` measures preliminary checks and non-GNFS algorithms. `sqrt_s` excludes the measured GCD extraction work recorded in `extract_s`. `dependencies_tried` includes attempted XOR combinations. Phase timings need not sum exactly to total wall time because setup, callbacks, and final validation also take time.
- `dependencies_found` counts only solver dependencies whose selected rows XOR to zero over the final matrix. The matrix phase drops every other dependency and emits a `WARN` log for each one.
- Complete-mode counters and phase times sum across splits. Worker counts use the maximum. Matrix rows, columns, weight, and excess come from the same matrix: the one with the largest row count, retaining the first on ties.
- Cancelled and failed runs preserve elapsed time at termination, even if the last progress event was much earlier. Only an accepted, verified successful result sets the GUI phase to Done.

## Validation

Backend coverage lives in `test_api`, `test_event_stream`, and `test_cli_event_stream`. It covers exact decimal digits, inclusive sieve widths, recursive trial/Rho results, actual GNFS applicability, reduction statistics, dependency attempts, and event boundaries. Workbench tests cover phase transitions, sample retention, unsigned counter decoding, environment isolation, real engine results, history compatibility, and parameter transactions.

Use the project API module and gate for the C++ changes. The separate Workbench runner owns Swift unit, real-process, offscreen rendering, and UI tests:

```bash
./scripts/test.sh -t Release module api
./scripts/test.sh -t Release --no-build gate
apps/macos/GNFSWorkbench/scripts/test-app.sh
apps/macos/GNFSWorkbench/scripts/test-app.sh --ui
```
