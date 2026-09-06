#!/usr/bin/env python3
"""Validate the small set of contracts that keeps the GNFS agent Harness coherent."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


EXPECTED_HOOK = "${CLAUDE_PROJECT_DIR}/.claude/hooks/project-guard.py"
CANONICAL_SKILLS = ("build-test", "gnfs-status")
SPECIALIST_AGENTS = ("gnfs-debugger", "gnfs-reviewer")
MAC_HOME = "/" + "Users" + "/"
LINUX_HOME = "/" + "home" + "/"
LOCAL_PATH_PATTERNS = (
    re.compile(re.escape(MAC_HOME) + r"[^<\s]+"),
    re.compile(re.escape(LINUX_HOME) + r"[^<\s]+"),
    re.compile(r"[A-Za-z]:\\Users\\[^<\s]+"),
)
STALE_MARKERS = (".Co" + "dex/", "make -C " + MAC_HOME, "noreply@" + "anthropic.com")
OBSOLETE_HARNESS_FILES = (
    ".claude/hooks.json",
    ".claude/hooks/stop-todo-check.sh",
    "tests/test_stop_todo_hook.sh",
    "TODO.md.example",
)
SHELL_FUNCTION_SHA256 = {
    ("scripts/test.sh", "run_single_test"): (
        "55c3e738e04fa08c35c3743d434390f1e23b7988dd7b5eab25d66278c5435920"
    ),
    ("scripts/test.sh", "run_dual_stream_with_timeout"): (
        "4597d13f41083f419ba7d8b436e3cf592de6fcf86ec0acf339d4cbdfeac1719b"
    ),
    ("scripts/lib/process_tree_timeout.zsh", "gnfs_run_process_supervisor"): (
        "1b2095568ca914b93a46d54d4fc4c1b448fc151a201744fbe109ce8c82eac339"
    ),
    ("scripts/lib/process_tree_timeout.zsh", "run_with_timeout"): (
        "d5e06613c0728fe7a3d8a55b53ffa544d8e5d25438f352169a41c991ae550837"
    ),
    ("scripts/lib/process_tree_timeout.zsh", "run_with_timeout_to_files"): (
        "6d720cd026803abd8d22b5f6aede197e696756fce955f384d3ec08c999736fc3"
    ),
}


class Checks:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.errors: list[str] = []

    def fail(self, message: str) -> None:
        self.errors.append(message)

    def read(self, relative: str) -> str:
        path = self.root / relative
        try:
            return path.read_text(encoding="utf-8")
        except OSError as exc:
            self.fail(f"{relative}: cannot read file: {exc}")
            return ""

    def shell_function_body(self, relative: str, text: str, name: str) -> str:
        match = re.search(rf"(?ms)^{re.escape(name)}\(\)\s*\{{\n(.*?)^\}}", text)
        if match is None:
            self.fail(f"{relative}: missing shell function {name}")
            return ""
        return match.group(1)

    def check_shell_function_digest(self, relative: str, text: str, name: str) -> str:
        body = self.shell_function_body(relative, text, name)
        expected = SHELL_FUNCTION_SHA256[(relative, name)]
        actual = hashlib.sha256(body.encode("utf-8")).hexdigest()
        if actual != expected:
            self.fail(f"{relative}: supervised execution route {name} changed")
        return body

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.root), *args],
            text=True,
            capture_output=True,
            check=False,
        )

    def check_executable(self, relative: str) -> None:
        path = self.root / relative
        if not path.is_file():
            self.fail(f"{relative}: missing")
            return

        staged = self.git("ls-files", "--stage", "--", relative)
        staged_mode = staged.stdout.split(maxsplit=1)[0] if staged.returncode == 0 else ""
        if staged_mode != "100755":
            self.fail(f"{relative}: must be tracked as executable")
            return

        # Windows worktrees do not materialize Git's POSIX executable bit.
        # The index mode is the portable source of truth there; POSIX hosts
        # additionally need the live mode because hooks execute the file
        # directly from the worktree.
        if os.name != "nt" and path.stat().st_mode & 0o111 == 0:
            self.fail(f"{relative}: must be executable")

    def check_instructions(self) -> None:
        agents = self.read("AGENTS.md")
        if len(agents.splitlines()) > 200:
            self.fail("AGENTS.md: keep shared startup instructions at or below 200 lines")

        claude = self.read("CLAUDE.md")
        if "@AGENTS.md" not in claude.splitlines():
            self.fail("CLAUDE.md: import the shared instructions with a standalone @AGENTS.md line")
        if len(claude.splitlines()) > 20:
            self.fail("CLAUDE.md: keep the Claude entry point thin instead of duplicating AGENTS.md")

    def check_settings(self) -> None:
        raw = self.read(".claude/settings.json")
        try:
            settings = json.loads(raw)
        except json.JSONDecodeError as exc:
            self.fail(f".claude/settings.json: invalid JSON: {exc}")
            return

        if settings.get("$schema") != "https://json.schemastore.org/claude-code-settings.json":
            self.fail(".claude/settings.json: add the official Claude Code JSON schema")

        hooks = settings.get("hooks")
        if not isinstance(hooks, dict):
            self.fail(".claude/settings.json: hooks must be an object")
            return

        pre_groups = hooks.get("PreToolUse", [])
        if not any(
            isinstance(group, dict)
            and set(str(group.get("matcher", "")).split("|")) == {"Edit", "Write"}
            and self.group_uses_guard(group, 5)
            for group in pre_groups
        ):
            self.fail(".claude/settings.json: PreToolUse must guard Edit and Write with project-guard.py")

        stop_groups = hooks.get("Stop", [])
        if not any(isinstance(group, dict) and self.group_uses_guard(group, 10) for group in stop_groups):
            self.fail(".claude/settings.json: Stop must run project-guard.py")

        for relative in OBSOLETE_HARNESS_FILES:
            if (self.root / relative).exists():
                self.fail(f"{relative}: remove obsolete Harness artifact")

    @staticmethod
    def group_uses_guard(group: dict, max_timeout: int) -> bool:
        handlers = group.get("hooks", [])
        return any(
            isinstance(handler, dict)
            and handler.get("type") == "command"
            and handler.get("command") == EXPECTED_HOOK
            and handler.get("args") == []
            and isinstance(handler.get("timeout"), int)
            and handler["timeout"] <= max_timeout
            for handler in handlers
        )

    def check_components(self) -> None:
        if not (self.root / "docs" / "harness-engineering.md").is_file():
            self.fail("docs/harness-engineering.md: missing Harness ownership and contract documentation")

        self.check_executable(".claude/hooks/project-guard.py")

        for name in CANONICAL_SKILLS:
            canonical_rel = f".claude/skills/{name}/SKILL.md"
            adapter_rel = f".agents/skills/{name}/SKILL.md"
            canonical = self.read(canonical_rel)
            adapter = self.read(adapter_rel)
            expected_reference = f"../../../.claude/skills/{name}/SKILL.md"
            if not canonical.startswith("---\n"):
                self.fail(f"{canonical_rel}: missing YAML frontmatter")
            if expected_reference not in adapter:
                self.fail(f"{adapter_rel}: must reference the canonical Claude skill")
            canonical_header = self.frontmatter(canonical)
            adapter_header = self.frontmatter(adapter)
            for key in ("name", "description"):
                if canonical_header.get(key) != adapter_header.get(key):
                    self.fail(f"{adapter_rel}: {key} must match the canonical skill")

        for name in SPECIALIST_AGENTS:
            relative = f".claude/agents/{name}.md"
            text = self.read(relative)
            header = self.frontmatter(text)
            for key in ("name", "description", "tools", "model", "permissionMode"):
                if key not in header:
                    self.fail(f"{relative}: missing frontmatter field {key}")
            tools = {tool.strip() for tool in header.get("tools", "").split(",")}
            if {"Edit", "Write"} & tools:
                self.fail(f"{relative}: specialist agents must remain read-only")

        self.check_executable("tests/test_harness_hooks.sh")

    def check_integration(self) -> None:
        cmake = self.read("CMakeLists.txt")
        if "add_test(NAME HarnessHooks" not in cmake or "tests/test_harness_hooks.sh" not in cmake:
            self.fail("CMakeLists.txt: register the HarnessHooks CTest")
        if (
            "add_test(NAME DistributedSievePolicyInventory" not in cmake
            or "scripts/check_distributed_sieve_policy.py" not in cmake
        ):
            self.fail("CMakeLists.txt: register the distributed-sieve policy inventory CTest")
        for marker in (
            "add_executable(gnfs_test_process_supervisor",
            "add_test(NAME ProcessSupervisor",
            "add_test(NAME HarnessProcessTreeTimeout",
        ):
            if marker not in cmake:
                self.fail(f"CMakeLists.txt: missing process-tree supervisor integration {marker!r}")

        test_runner = self.read("scripts/test.sh")
        timeout_library = self.read("scripts/lib/process_tree_timeout.zsh")
        if 'source "${PROJECT_ROOT}/scripts/lib/process_tree_timeout.zsh"' not in test_runner:
            self.fail("scripts/test.sh: source the shared process-tree timeout wrapper")
        for marker in (
            "gnfs_test_process_supervisor_path",
            "gnfs_run_process_supervisor",
            "setopt localtraps trapsasync",
            "unsetopt bgnice",
            "--combined-output",
            "--stdout-file",
            "--stderr-file",
        ):
            if marker not in timeout_library:
                self.fail(
                    f"scripts/lib/process_tree_timeout.zsh: missing supervisor route {marker!r}"
                )
        expected_kill_lines = (
            'kill -"$signal_name" "$gnfs_active_supervisor_pid" 2>/dev/null || true',
            'kill -"$gnfs_forwarded_signal_name" "$gnfs_active_supervisor_pid" '
            "2>/dev/null || true",
            'kill -"$signal_name" "$shell_pid" 2>/dev/null || return 125',
        )
        observed_kill_lines: list[str] = []
        for line_number, line in enumerate(timeout_library.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("#") or not re.search(r"\bkill\b", stripped):
                continue
            observed_kill_lines.append(stripped)
            if stripped not in expected_kill_lines:
                self.fail(
                    "scripts/lib/process_tree_timeout.zsh:"
                    f"{line_number}: unexpected supervisor signal command"
                )
        if sorted(observed_kill_lines) != sorted(expected_kill_lines):
            self.fail(
                "scripts/lib/process_tree_timeout.zsh: supervisor signal commands must be exact"
            )
        for wrapper in ("run_with_timeout", "run_with_timeout_to_files"):
            definition = rf"(?m)^{wrapper}\(\)\s*\{{"
            if len(re.findall(definition, timeout_library)) != 1 or re.search(
                definition, test_runner
            ):
                self.fail(f"{wrapper} must have one canonical library definition")
            wrapper_body = self.check_shell_function_digest(
                "scripts/lib/process_tree_timeout.zsh", timeout_library, wrapper
            )
            if wrapper_body.count("gnfs_run_process_supervisor ") != 1:
                self.fail(f"scripts/lib/process_tree_timeout.zsh: {wrapper} must use supervisor")
        self.check_shell_function_digest(
            "scripts/lib/process_tree_timeout.zsh",
            timeout_library,
            "gnfs_run_process_supervisor",
        )

        single_test = self.check_shell_function_digest(
            "scripts/test.sh", test_runner, "run_single_test"
        )
        single_route = 'run_with_timeout "$test_timeout" "$binary" "${extra_args[@]}"'
        if single_test.count(single_route) != 2:
            self.fail("scripts/test.sh: run_single_test and its retry must use run_with_timeout")
        if "$!" in single_test or re.search(r"(?m)^\s*(?:kill|wait)\b", single_test):
            self.fail("scripts/test.sh: run_single_test must not supervise raw child PIDs")
        dual_test = self.check_shell_function_digest(
            "scripts/test.sh", test_runner, "run_dual_stream_with_timeout"
        )
        if 'run_with_timeout_to_files "$stdout_file" "$stderr_file" "$timeout_seconds"' not in dual_test:
            self.fail(
                "scripts/test.sh: dual-stream consumer must use run_with_timeout_to_files"
            )
        if "$!" in dual_test or re.search(r"(?m)^\s*(?:kill|wait)\b", dual_test):
            self.fail("scripts/test.sh: dual-stream consumer must not supervise raw child PIDs")
        for mode in ("do_smoke", "do_gate"):
            body = self.shell_function_body("scripts/test.sh", test_runner, mode)
            if '"$SMOKE_CTEST_CONTRACT"' not in body:
                self.fail(f"scripts/test.sh: {mode} must run the smoke CTest contract")
        for legacy_marker in ('child_pid=$!', 'wait "$child_pid"', 'kill "$child_pid"'):
            if legacy_marker in test_runner:
                self.fail(f"scripts/test.sh: legacy raw-PID timeout marker {legacy_marker!r}")

        workflow = self.read(".github/workflows/scripts.yml")
        for marker in ("tests/*.zsh", "scripts/lib/*.zsh", "NO_BG_NICE"):
            if marker not in workflow:
                self.fail(f".github/workflows/scripts.yml: missing zsh coverage {marker!r}")
        for command in (
            "python3 scripts/check_distributed_sieve_policy.py --self-test",
            "python3 scripts/check_harness.py",
            "bash tests/test_harness_hooks.sh",
        ):
            if command not in workflow:
                self.fail(f".github/workflows/scripts.yml: missing CI command {command!r}")

        release_readiness = self.read(".github/workflows/release-readiness.yml")
        for marker in (
            "Verify MinGW process-tree supervisor",
            "-DGNFS_BUILD_TESTS=ON",
            "--target gnfs_test_process_supervisor",
            "-R '^ProcessSupervisor$'",
        ):
            if marker not in release_readiness:
                self.fail(
                    ".github/workflows/release-readiness.yml: "
                    f"missing MinGW supervisor coverage {marker!r}"
                )

    @staticmethod
    def frontmatter(text: str) -> dict[str, str]:
        lines = text.splitlines()
        if not lines or lines[0] != "---":
            return {}
        result: dict[str, str] = {}
        for line in lines[1:]:
            if line == "---":
                break
            if ":" in line:
                key, value = line.split(":", 1)
                result[key.strip()] = value.strip()
        return result

    def check_git_contracts(self) -> None:
        tracked_result = self.git("ls-files", "-z")
        if tracked_result.returncode != 0:
            self.fail(f"git ls-files failed: {tracked_result.stderr.strip()}")
            return
        tracked = set(tracked_result.stdout.split("\0"))

        for relative in OBSOLETE_HARNESS_FILES:
            if relative in tracked and (self.root / relative).exists():
                self.fail(f"{relative}: obsolete Harness artifact must not be tracked")

        for local_file in (".claude/settings.local.json", "CLAUDE.local.md"):
            if local_file in tracked:
                self.fail(f"{local_file}: personal settings must not be tracked")
            ignored = self.git("check-ignore", "--no-index", "-q", local_file)
            if ignored.returncode != 0:
                self.fail(f"{local_file}: add personal settings to .gitignore")

        adapter = ".agents/skills/build-test/SKILL.md"
        if adapter not in tracked:
            self.fail(f"{adapter}: cross-tool skill adapter must be tracked")
        if self.git("check-ignore", "--no-index", "-q", adapter).returncode == 0:
            self.fail(f"{adapter}: build artifact ignore patterns must not hide this skill")

    def check_portability(self) -> None:
        tracked_result = self.git("ls-files")
        tracked = tracked_result.stdout.splitlines() if tracked_result.returncode == 0 else []
        text_suffixes = {".cmake", ".cpp", ".hpp", ".json", ".md", ".py", ".sh", ".txt", ".yaml", ".yml", ".zsh"}
        candidates = [
            path
            for path in tracked
            if path in {"CMakeLists.txt", ".gitignore"} or Path(path).suffix in text_suffixes
        ]
        candidates.extend(["scripts/check_harness.py", ".claude/hooks/project-guard.py"])

        for relative in sorted(set(candidates)):
            if relative.startswith(".claude/worktrees/"):
                continue
            if not (self.root / relative).is_file():
                continue
            text = self.read(relative)
            for pattern in LOCAL_PATH_PATTERNS:
                if pattern.search(text):
                    self.fail(f"{relative}: contains a machine-specific absolute path")
                    break
            if relative in {"AGENTS.md", "CLAUDE.md"} or relative.startswith((".claude/", ".agents/")):
                for marker in STALE_MARKERS:
                    if marker in text:
                        self.fail(f"{relative}: contains stale Harness marker {marker!r}")

    def run(self) -> list[str]:
        self.check_instructions()
        self.check_settings()
        self.check_components()
        self.check_integration()
        self.check_git_contracts()
        self.check_portability()
        return self.errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quiet", action="store_true", help="print only failures")
    parser.add_argument("--root", type=Path, help="repository root (defaults to this script's parent)")
    args = parser.parse_args()

    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    errors = Checks(root).run()
    if errors:
        for error in errors:
            print(f"[FAIL] {error}", file=sys.stderr)
        return 1
    if not args.quiet:
        print("[PASS] GNFS Harness contracts are coherent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
