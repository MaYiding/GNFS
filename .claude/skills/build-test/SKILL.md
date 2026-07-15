---
name: build-test
description: Build GNFS or run a requested test mode through the project test runner. Use when the user asks to compile, test, run smoke/gate/E2E, or verify a code change.
argument-hint: "[smoke|changed|changed --deep|module <name>|run <test>|gate|e2e|build]"
---

# GNFS Build and Test

Use `scripts/test.sh` as the only normal entry point. It owns CMake configuration, build parallelism, per-test timeouts, and test reporting.

## Select the mode

- No argument: run `./scripts/test.sh smoke`.
- A supported mode in `$ARGUMENTS`: pass that mode and its arguments to `./scripts/test.sh`.
- A bare test name: run `./scripts/test.sh run <test_name>`.
- An unknown or ambiguous request: run `./scripts/test.sh list` and select the narrowest matching mode.

Do not silently upgrade a request to `full`, `thorough`, `nightly`, benchmark, progressive, or stress testing. Those modes are expensive and require an explicit request or clear risk justification.

## Run and report

1. Run `git status --short --branch` so pre-existing changes are visible.
2. Execute the selected `scripts/test.sh` command from the repository root.
3. If it fails, preserve the first useful error and the runner's timeout context. Diagnose or fix only to the extent authorized by the user's request.
4. Report the exact command, build type, passed/failed result, and any skipped or unrun coverage.

Use direct `cmake`, `ctest`, or test binaries only to diagnose the unified runner or to reproduce a case the runner cannot express. Explain that exception in the result.
