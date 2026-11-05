# GNFS Workbench Design QA

## Current Direction

The interface uses native macOS controls and system light or dark appearance, with the information density of a computation workbench. A searchable history sidebar, compact run header, stage-aware metrics, and structured statistics replace the original fixed dark canvas.

The earlier direction-2 comparison images in `evidence/` document the previous iteration. They are historical design material, not acceptance evidence for the current implementation.

## Information Hierarchy

- Draft: integer input, examples, requested algorithm, and validated parameter editing.
- Running: current algorithm and remainder, elapsed time, applicable stage metrics, and effective GNFS parameters.
- Completed: verified factors, exact expression, task statistics, individual split selection, and logs.
- Failed or cancelled: the observed last phase, terminal elapsed time, available statistics, and a reusable configuration.

Requested overrides and effective parameters have separate labels. Non-GNFS computation does not display a completed GNFS rail. Multiple splits do not collapse into one fictitious pipeline. Relations, matrices, timing, and algorithm labels follow the [telemetry contract](telemetry.md).

## Layout and Interaction Coverage

The native window supports a `900 × 640` minimum. The history sidebar can be hidden; the main area scrolls vertically. Metrics adapt to one or two columns, long integers wrap, and advanced parameters open in a transactional sheet.

Offscreen rendering tests cover ready, live, complete, failure, long-input, invalid-parameter, and full-sheet states. Light and dark captures include minimum-width windows and detailed GNFS statistics. These are deterministic fixtures for layout review, not performance measurements.

UI tests exercise initial launch, termination and relaunch, closing the final window and reopening, decimal input, running and cancellation states, parameter-sheet dismissal, malformed parameter validation, a real `360` factorization, configuration reuse, and history search. The main scene has a stable identifier and an explicit launch fallback for older, type-derived window restoration identifiers. Process tests independently verify actual engine results, environment isolation, and subprocess cleanup.

## Reproducing Evidence

Run the app-specific suites from the repository root:

```bash
GNFS_WORKBENCH_TEST_ARTIFACTS=/private/tmp/gnfs-workbench-headless \
  apps/macos/GNFSWorkbench/scripts/test-app.sh
GNFS_WORKBENCH_TEST_ARTIFACTS=/private/tmp/gnfs-workbench-ui \
  apps/macos/GNFSWorkbench/scripts/test-app.sh --ui
```

The explicit artifact directory preserves the test log, result bundle, and successful app build. Export PNG attachments from the result bundle with `xcresulttool` for visual review. Inspect actual test results before claiming acceptance; screenshot fixtures alone cannot validate engine measurements.
