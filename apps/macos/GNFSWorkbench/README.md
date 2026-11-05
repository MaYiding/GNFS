# GNFS Workbench for macOS

GNFS Workbench is the native macOS interface for the GNFS command-line engine. It combines a searchable history sidebar, multiline integer input, algorithm-aware progress, structured logs, verified factors, and transactional parameter editing in one window. The interface follows the system light or dark appearance and supports windows down to `900 × 640` logical pixels.

The app is built with SwiftUI and Swift Charts. It runs the bundled `gnfs` executable as an isolated child process and consumes the versioned `--event-stream` JSON Lines protocol. A failed engine process therefore does not take down the interface, and cancellation terminates the child process safely.

## Requirements

- macOS 26 or later on Apple silicon
- Xcode 26 or a compatible Swift 6 toolchain
- XcodeGen 2.45 or later
- CMake and the Homebrew `gmp` and `ntl` formulae, including their static libraries

## Build a distributable app

From the repository root:

```bash
apps/macos/GNFSWorkbench/scripts/build-app.sh Release
```

The script regenerates the Xcode project and builds only the matching C++ engine target. It statically links GMP and NTL, so the app has no Homebrew runtime dependency. It writes the full checkout commit to the `GNFSSourceRevision` bundle key before signing. Release packaging requires a clean checkout and verifies that the embedded revision equals the exact `HEAD` commit.

The package undergoes ad-hoc local signing. It is not Developer ID signed or notarized, so it is suitable for CI and local validation but not unrestricted public distribution. The script creates `GNFSWorkbench-0.1.0-macOS-arm64.zip` and `GNFSWorkbench-0.1.0-macOS-arm64.zip.sha256`. It extracts the temporary ZIP and rechecks the signatures, arm64 architecture, dynamic dependencies, version, source revision, license resources, and a real `360` JSONL factorization. An exclusive output lock protects the final publication transaction. A handled interruption restores the previous archive and checksum pair; an abrupt process death leaves a fail-closed lock for inspection.

The app bundle includes exactly six files under `Contents/Resources/Licenses/`: the GNFS GPLv2 license, the GMP GPLv2 and LGPLv3 texts, the NTL copyright and LGPLv2.1-or-later text, third-party notices, and exact source locations. Packaging verifies these files after ZIP extraction. This GPL-2.0 distribution conveys statically linked GMP 6.3.0 under its GNU GPL version 2 option.

The ZIP is the canonical handoff artifact because a raw `.app` left inside a FileProvider-synced workspace can acquire Finder metadata after signing. Extract the archive into `/Applications` or another non-synced directory. The generated `.xcodeproj` and distribution artifacts are intentionally not tracked. Each invocation creates unique C++, Xcode, and packaging scratch directories with deterministic cleanup, so concurrent CI jobs cannot share a build root or reconfigure the repository's normal test build.

For Xcode development:

```bash
apps/macos/GNFSWorkbench/scripts/generate-project.sh
open apps/macos/GNFSWorkbench/GNFSWorkbench.xcodeproj
```

## Validate without taking over the screen

The default suite is headless. It covers integer validation, exact large-integer result verification, invocation construction, event decoding, run-state transitions, cancellation, persistence, real child-process factorizations, and offscreen renders of the live dashboard, ready/result/error states, history, and parameter surfaces.

```bash
apps/macos/GNFSWorkbench/scripts/test-app.sh
```

Screen-driving UI tests are available separately, and Workbench CI runs them as
an independent release gate after the headless suite:

```bash
apps/macos/GNFSWorkbench/scripts/test-app.sh --ui
```

Both scripts generate their temporary Xcode project and derived data under `/private/tmp`. This keeps routine builds away from Desktop FileProvider metadata, avoids duplicate generated projects, and requires no signing account or manual approval. The explicit `generate-project.sh` command above still writes a local project beside `project.yml` when a developer wants to open it in Xcode.

Each test has a 120-second default execution allowance and a 300-second maximum. A separate 1,200-second wall timeout also bounds build or test-worker startup stalls; `GNFS_WORKBENCH_TEST_WALL_TIMEOUT_SECONDS` can override it. Set `GNFS_WORKBENCH_TEST_ARTIFACTS` to a local directory to preserve the Xcode result bundle, build log, and test logs for both passing and failing runs. Successful runs also preserve the tested Debug app. This opt-in output supports visual review of the attached offscreen PNGs. CI uploads this evidence automatically.

## Workbench Controls

- Enter a decimal or `0x` hexadecimal integer, or choose an example. Spaces, commas, and underscores are accepted as separators. Press Return or `Command-R` to start.
- Use `Command-N` to prepare another run and `Command-.` to stop the active engine. Escape closes the parameter sheet without cancelling a computation.
- Show or hide the history sidebar with its toolbar button or `Control-Command-S`. Search matches input, factors, algorithm, and status. Previous runs remain readable during a computation; a return button restores the active task.
- Open parameters with the toolbar or `Shift-Command-,`. Draft values are applied only after every field passes validation. A historical configuration opens read-only; **Edit Copy** creates a new draft without changing the original record. Cancelling the sheet discards its edits.
- The calculation statistics selector shows either the whole task or one composite split, including its actual algorithms, effective GNFS parameters, SIQS statistics, counters, and timings. Missing legacy telemetry is marked unavailable. See the [telemetry contract](../../../docs/apps/gnfs-workbench/telemetry.md).
- Progress uses only measurements supplied for the current stage. Algorithms other than GNFS show their actual method rather than marking unused GNFS stages complete. Elapsed time continues updating between engine events.
- Filter logs by text or severity, pause **Follow Latest** to read earlier entries, and copy either the filtered set or all retained entries. The buffer retains the latest 600 entries and continues following after the limit is reached.
- Export a selected run with the share button. The JSON snapshot contains the original input, configuration, result, metrics, and retained logs. The native save dialog lets the user choose its destination.

## Runtime behavior

- Automatic mode chooses trial division, Pollard Rho, SIQS, or GNFS from the exact input size. Explicit route selections still permit preliminary algorithms to finish first; the interface reports actual execution.
- Every GUI run recursively factors all composite remainders. A successful result contains the complete prime factorization, including repeated factors; prime input is returned as the sole prime factor.
- Advanced values are optional overrides; blank fields retain size-derived defaults. Malformed, fractional, negative, and out-of-range values block Apply; the run entry point separately revalidates the resulting configuration.
- Inherited `GNFS_*` engine tuning variables are removed from the child environment before the managed resume path is set, so external tuning cannot silently alter the GUI configuration.
- Each active run owns `Runs/<UUID>/` and passes `Runs/<UUID>/state` through `GNFS_RESUME`. The workspace supports checkpoints only within that process lifetime and is removed after success, failure, cancellation, or interrupted-run recovery.
- History is stored locally as JSON. Interrupted records are recovered as cancelled on the next launch. Malformed history and duplicate run IDs are quarantined. The recovery banner can retry loading without deleting the quarantined file.
- Deleting history, clearing all history, and the 50-record retention limit remove exact UUID workspace directories and known legacy checkpoint suffixes. Unknown files and symbolic links are preserved and cause unsafe managed paths to fail closed.
- A successful result is accepted only when every returned factor passes the engine's primality check and their exact arbitrary-precision product equals `N`, and the returned number equals the requested decimal or hexadecimal input.
- Closing the application during a run first cancels the engine and persists the terminal state.

The app sandbox is disabled because GNFS Workbench must launch its bundled computation process. Hardened runtime remains enabled for normal builds.

The visual design record and offscreen review evidence live in
[`docs/apps/gnfs-workbench/design-qa.md`](../../../docs/apps/gnfs-workbench/design-qa.md).
