# GNFS Workbench for macOS

GNFS Workbench is the native macOS interface for the GNFS command-line engine. It presents method selection, the eight-stage factorization pipeline, live relation metrics, structured logs, verified factors, and local run history in one window.

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

The package undergoes ad-hoc local signing. It is not Developer ID signed or notarized, so it is suitable for CI and local validation but not unrestricted public distribution. The script creates `GNFSWorkbench-0.1.0-macOS-arm64.zip` and `GNFSWorkbench-0.1.0-macOS-arm64.zip.sha256`. It extracts the temporary ZIP and rechecks the signatures, arm64 architecture, dynamic dependencies, version, source revision, and a real `360` JSONL factorization before atomically publishing both files.

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

Screen-driving UI tests are available only when they are explicitly useful:

```bash
apps/macos/GNFSWorkbench/scripts/test-app.sh --ui
```

Both scripts generate their temporary Xcode project and derived data under `/private/tmp`. This keeps routine builds away from Desktop FileProvider metadata, avoids duplicate generated projects, and requires no signing account or manual approval. The explicit `generate-project.sh` command above still writes a local project beside `project.yml` when a developer wants to open it in Xcode.

## Runtime behavior

- Automatic mode chooses trial division, Pollard Rho, SIQS, or GNFS from the input size. Every method can also be selected explicitly.
- Every GUI run recursively factors all composite remainders. A successful result contains the complete prime factorization, including repeated factors; prime input is returned as the sole prime factor.
- Advanced values are optional overrides; blank fields retain size-derived defaults.
- Each active run owns `Runs/<UUID>/` and passes `Runs/<UUID>/state` through `GNFS_RESUME`. The workspace supports checkpoints only within that process lifetime and is removed after success, failure, cancellation, or interrupted-run recovery.
- History is stored locally as JSON. Interrupted records are recovered as cancelled on the next launch.
- Deleting history, clearing all history, and the 50-record retention limit also remove matching workspaces, legacy `UUID.*` checkpoints, and orphaned run artifacts.
- A successful result is accepted only when every returned factor passes the engine's primality check and their exact arbitrary-precision product equals `N`.
- Closing the application during a run first cancels the engine and persists the terminal state.

The app sandbox is disabled because GNFS Workbench must launch its bundled computation process. Hardened runtime remains enabled for normal builds.

The visual design record and offscreen review evidence live in
[`docs/apps/gnfs-workbench/design-qa.md`](../../../docs/apps/gnfs-workbench/design-qa.md).
