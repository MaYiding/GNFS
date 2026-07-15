#!/usr/bin/env python3
"""Validate the small set of contracts that keeps the GNFS agent Harness coherent."""

from __future__ import annotations

import argparse
import json
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

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.root), *args],
            text=True,
            capture_output=True,
            check=False,
        )

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

        legacy = self.root / ".claude" / "hooks.json"
        if legacy.exists():
            self.fail(".claude/hooks.json: remove the unused legacy hook configuration")

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
        hook = self.root / ".claude" / "hooks" / "project-guard.py"
        if not hook.is_file():
            self.fail(".claude/hooks/project-guard.py: missing")
        elif hook.stat().st_mode & 0o111 == 0:
            self.fail(".claude/hooks/project-guard.py: must be executable")

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
        candidates = [
            path
            for path in tracked
            if path in {"AGENTS.md", "CLAUDE.md", ".gitignore"}
            or path.startswith((".claude/", ".agents/"))
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
            for marker in STALE_MARKERS:
                if marker in text:
                    self.fail(f"{relative}: contains stale Harness marker {marker!r}")

    def run(self) -> list[str]:
        self.check_instructions()
        self.check_settings()
        self.check_components()
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
