# GNFS Harness Engineering

This document defines the repository's agent Harness: shared instructions, Claude Code integration, reusable skills, specialized agents, deterministic Hooks, and the checks that keep them coherent.

The Harness exists to prevent repeatable mechanical mistakes and to expose live project workflows. It does not replace engineering judgment, code review, or risk-based testing.

The project Harness requires Python 3. The GNFS C++ library and CLI do not acquire a Python runtime dependency.

## Design Principles

1. **One shared instruction source.** `AGENTS.md` contains cross-tool project facts and invariants. `CLAUDE.md` imports it instead of maintaining a second copy.
2. **Dynamic facts stay dynamic.** Test names, tiers, counts, and timeouts come from `scripts/test.sh` and `docs/testing-ci-policy.md`; agent instructions do not snapshot them.
3. **Procedures load on demand.** Build and status workflows live in skills, not startup instructions.
4. **Specialists are read-only.** The reviewer and debugger gather evidence and return findings. The main agent remains responsible for edits.
5. **Hooks enforce only cheap facts.** Hooks do not compile, run broad tests, access the network, invoke models, commit, or push.
6. **Personal configuration stays local.** Shared settings contain project Hooks; permissions and machine-specific preferences remain untracked.

These boundaries follow Claude Code's official separation between [project memory](https://code.claude.com/docs/en/memory), [settings](https://code.claude.com/docs/en/settings), [skills](https://code.claude.com/docs/en/skills), [subagents](https://code.claude.com/docs/en/sub-agents), and [Hooks](https://code.claude.com/docs/en/hooks).

## File Ownership

| File or directory | Role | Loaded when |
|---|---|---|
| `AGENTS.md` | Cross-tool GNFS rules and invariants | Every agent session |
| `CLAUDE.md` | Thin Claude Code entry point | Claude Code session start |
| `.claude/settings.json` | Team-shared Hook registration | Claude Code project load |
| `.claude/settings.local.json` | Personal permissions and experiments | Local only; never tracked |
| `.claude/skills/` | Canonical GNFS procedures | Explicit invocation or relevant request |
| `.agents/skills/` | Thin adapters for other agent hosts | Host-specific skill discovery |
| `.claude/agents/` | Read-only GNFS reviewer and debugger | Explicit or task-driven delegation |
| `.claude/hooks/project-guard.py` | Claude lifecycle guard | `PreToolUse` and `Stop` events |
| `scripts/check_harness.py` | Cross-tool deterministic self-check | Hook, CI, or manual validation |
| `tests/test_harness_hooks.sh` | Hook behavior regression tests | CTest and script CI |

Do not add a second settings file, duplicate skill procedure, or another unconditional instruction file for the same concern.

## Hook Contracts

### Write Boundary

The `PreToolUse` Hook examines `Edit` and `Write` calls. It denies direct edits to:

- root build directories: `build/`, `build-*`, `cmake-build-*`, and `xcode-build/`;
- any `CMakeFiles/` directory;
- project-managed `.worktrees/` and `.claude/worktrees/` directories.

The guard resolves paths relative to `${CLAUDE_PROJECT_DIR}` and checks path components. It therefore does not repeat the former `.gitignore` bug where `build-*/` also matched `.agents/skills/build-test/`.

The Hook does not block `Bash`: builds legitimately write generated output through shell commands. Its purpose is to stop an agent from treating generated files as source, not to prevent generators from running.

### Stop Integrity Check

The `Stop` Hook runs:

```bash
python3 scripts/check_harness.py --quiet
```

A passing check emits no output. A failing check returns `decision: "block"` with the deterministic errors so Claude can repair them. When `stop_hook_active` is already true, the Hook emits no decision; this prevents continuation loops if the failure cannot be repaired in the current session.

The checker must remain fast and offline. Adding compilation, CTest, network access, or semantic review to this path is out of scope.

## Self-Check Coverage

`scripts/check_harness.py` verifies:

- the shared instruction file remains at or below 200 lines;
- `CLAUDE.md` stays thin and imports `AGENTS.md`;
- `.claude/settings.json` is valid JSON, declares the official schema, and registers the current Hook contract;
- Hook scripts are present and executable;
- canonical skills and cross-tool adapters are connected;
- specialist agents have required frontmatter and no edit tools;
- personal settings are ignored and untracked;
- build ignore patterns do not hide `build-test` skills;
- active Harness files contain no machine-specific absolute paths or stale configuration markers;
- removed legacy Hook and TODO-gate artifacts do not reappear.

The checker tests structural contracts, not prose quality or algorithm correctness. Keep subjective review out of it.

## Validation

Run the complete Harness validation from the repository root:

```bash
python3 scripts/check_harness.py
bash tests/test_harness_hooks.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
ctest --test-dir build -R '^HarnessHooks$' --output-on-failure
claude doctor
git diff --check
```

`claude doctor` validates the installed Claude Code version and project configuration. The repository tests independently validate behavior so CI does not depend on authentication or a live model.

## Change Checklist

When changing the Harness:

- update the single owning file instead of copying information elsewhere;
- add or update a deterministic test for Hook behavior;
- keep Hook paths relative to `${CLAUDE_PROJECT_DIR}` and use exec-form `args`;
- preserve fail-open behavior for malformed non-security input and fail-closed behavior for generated-file edits;
- keep Stop checks below the configured timeout with no external services;
- run the validation commands above;
- update this document only when ownership or contracts change.

## Restraint Test

Add a rule, skill, agent, or Hook only when at least one of these is true:

- the same agent mistake has recurred;
- a GNFS invariant is easy to forget and costly to violate;
- a repeated workflow has a stable project-specific command sequence;
- a cheap deterministic check can replace unreliable prompt compliance.

Do not add Harness machinery for a one-off preference, an unmeasured optimization idea, a generic coding principle, or a check already owned by CMake, the test runner, or CI.
