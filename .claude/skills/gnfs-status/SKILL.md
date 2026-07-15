---
name: gnfs-status
description: Show a concise GNFS repository health snapshot. Use only when the user explicitly invokes this skill or asks for project status.
argument-hint: "[quick|verify]"
---

# GNFS Project Status

Report current facts without presenting cached results as fresh verification.

## Quick mode

Use quick mode when `$ARGUMENTS` is empty or contains `quick`:

1. Run `git status --short --branch`.
2. Run `git log -1 --oneline --decorate`.
3. Check whether `build/CMakeCache.txt` exists. If it does, report the configured `CMAKE_BUILD_TYPE`; otherwise report that the build is not configured.
4. If `build/Testing/Temporary/LastTest.log` or `build/test_report.json` exists, report its modification time as historical evidence. Do not call it a current pass.

## Verify mode

When `$ARGUMENTS` contains `verify`, run `./scripts/test.sh smoke` after the Git snapshot. This performs a current build and instant-tier verification.

## Output

Summarize:

- branch and working-tree state;
- build configuration;
- current verification result, or an explicit "not verified in this run";
- most recent commit;
- the next smallest useful command if health is uncertain.

Do not calculate file or line counts; they are not a health signal and become stale immediately.
