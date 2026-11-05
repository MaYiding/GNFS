# GNFS Workbench Design QA

## Current Direction

The interface uses a native macOS window toolbar and a continuous document surface. A compact history sidebar, restrained blue accent, and neutral light or dark palette organize the work without nested dashboard cards. The input page contains the composer, examples, and method selection; calculation details appear after a run starts.

The earlier direction-2 comparison images in `evidence/` document the previous iteration. They are historical design material, not acceptance evidence for the current implementation.

## Information Hierarchy

- Draft: integer input, examples, requested algorithm, and validated parameter editing.
- Running: current algorithm and remainder, elapsed time, applicable stage metrics, and effective GNFS parameters.
- Completed: verified factors, exact expression, task statistics, individual split selection, and logs.
- Failed or cancelled: the observed last phase, terminal elapsed time, available statistics, and a reusable configuration.

Requested overrides and effective parameters have separate labels. Non-GNFS computation does not display a completed GNFS rail. Multiple splits do not collapse into one fictitious pipeline. Relations, matrices, timing, and algorithm labels follow the [telemetry contract](telemetry.md).

## Layout and Interaction Coverage

The native window supports a `900 × 640` minimum. A native split view contains the history sidebar. Hiding and reopening it preserves the query and selection. Selecting another record returns the document to its top. Long inputs and complete factor expressions wrap without horizontal scrolling. Verified repeated factors use superscript exponents while copy operations preserve the exact machine-readable expression. Statistics use aligned rows with adaptive columns, and advanced parameters open in a transactional sheet. Logs start collapsed and retain their search, severity, copy, and follow controls when expanded. The console keeps a fixed height when filtering. Reading older entries pauses automatic following; the latest-entry control resumes it.

Offscreen rendering tests cover ready, live, complete, failure, long-input, invalid-parameter, and full-sheet states. Light and dark captures include minimum-width windows and detailed GNFS statistics. These are deterministic fixtures for layout review, not performance measurements.

UI tests exercise initial launch, termination and relaunch, closing the final window and reopening, decimal input, running and cancellation states, parameter-sheet dismissal, malformed parameter validation, a real `360` factorization, configuration reuse, and history search. The main scene has a stable identifier and an explicit launch fallback for older, type-derived window restoration identifiers. Process tests independently verify actual engine results, environment isolation, and subprocess cleanup.

## Component and Motion Contract

- Control hover and press feedback changes the surface without scaling labels or moving neighboring controls. Focus rings remain visible; disabled controls cannot trigger actions.
- Sidebar and disclosure changes use a short, interruptible transition. Record changes fade without interpolating values between different integers. All custom motion respects Reduce Motion.
- Live counts, times, chart samples, and effective parameters update directly. Only the changing state indicator animates. Progress is never inferred from an animation.
- The integer editor supports Return to submit, modified Return for text editing, and marked text composition. Validation occupies a reserved feedback row.
- Parameter edits remain transactional. Read-only records show legible values; editing requires a new draft. Cancel and Escape preserve the previous draft.
- History rows support pointer feedback and arrow-key navigation. Search and selection survive sidebar disclosure. Selecting a record brings it into view.
- Log disclosure preserves filters. Filtering and following are separate actions; new events do not pull the reader away from older messages.

Native regression coverage includes repeated sidebar disclosure, Return submission, log expansion and filter persistence, and keyboard history selection. Offscreen fixtures additionally cover Reduce Motion and a console at the retained-log limit. Native runtime checks are required to assess transitions; static screenshots alone do not establish animation quality.

## Reproducing Evidence

Run the app-specific suites from the repository root:

```bash
GNFS_WORKBENCH_TEST_ARTIFACTS=/private/tmp/gnfs-workbench-headless \
  apps/macos/GNFSWorkbench/scripts/test-app.sh
GNFS_WORKBENCH_TEST_ARTIFACTS=/private/tmp/gnfs-workbench-ui \
  apps/macos/GNFSWorkbench/scripts/test-app.sh --ui
```

The explicit artifact directory preserves the test log, result bundle, and successful app build. Export PNG attachments from the result bundle with `xcresulttool` for visual review. Inspect actual test results before claiming acceptance; screenshot fixtures alone cannot validate engine measurements.
