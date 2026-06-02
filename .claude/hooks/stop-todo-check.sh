#!/usr/bin/env bash
# Stop Hook: block conversation stop while user-owned TODO.md has open tasks.
#
# TODO.md config:
#   stop_hook_prevent_infinite_loop: true   # default, allow a second stop attempt
#   stop_hook_prevent_infinite_loop: false  # strict mode, block until tasks are done

set -euo pipefail

INPUT=$(cat)
export INPUT

python3 <<'PY'
import json
import os
import pathlib
import re
import sys


TRUE_VALUES = {"1", "true", "yes", "on"}
FALSE_VALUES = {"0", "false", "no", "off"}


def load_input(raw: str) -> dict:
    if not raw.strip():
        return {}
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return data if isinstance(data, dict) else {}


def as_bool(value) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in TRUE_VALUES
    return bool(value)


def parse_guard(lines: list[str]) -> bool:
    pattern = re.compile(
        r"""
        ^\s*
        (?:<!--\s*)?
        (?:STOP_HOOK_PREVENT_INFINITE_LOOP|stop_hook_prevent_infinite_loop|stop-hook-prevent-infinite-loop)
        \s*[:=]\s*
        (?P<value>true|false|1|0|yes|no|on|off)
        \b
        """,
        re.IGNORECASE | re.VERBOSE,
    )

    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        value = match.group("value").strip().lower()
        if value in TRUE_VALUES:
            return True
        if value in FALSE_VALUES:
            return False
    return True


def collect_tasks(lines: list[str]) -> tuple[list[str], int]:
    task_pattern = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+\[(?P<mark>[ xX])\]\s*(?P<body>.*)$")
    incomplete: list[str] = []
    complete_count = 0

    for line in lines:
        match = task_pattern.match(line)
        if not match:
            continue
        mark = match.group("mark")
        if mark == " ":
            incomplete.append(line.strip()[:100])
        else:
            complete_count += 1

    return incomplete, complete_count


data = load_input(os.environ.get("INPUT", ""))
cwd_raw = data.get("cwd") or os.getcwd()
todo_file = pathlib.Path(str(cwd_raw)).expanduser() / "TODO.md"

if not todo_file.is_file():
    sys.exit(0)

try:
    lines = todo_file.read_text(encoding="utf-8", errors="replace").splitlines()
except OSError:
    sys.exit(0)

prevent_infinite_loop = parse_guard(lines)
stop_hook_active = as_bool(data.get("stop_hook_active", False))

if prevent_infinite_loop and stop_hook_active:
    sys.exit(0)

incomplete, complete_count = collect_tasks(lines)
if not incomplete:
    sys.exit(0)

guard_line = (
    "防循环守卫：开启（第二次 Stop 尝试会放行）。"
    if prevent_infinite_loop
    else "防循环守卫：关闭（会持续阻止 Stop，直到 TODO.md 全部完成）。"
)
items = "\n".join(incomplete[:10])
reason = (
    f"【Stop Hook】TODO.md 中仍有 {len(incomplete)} 项未完成（已完成 {complete_count} 项）。\n\n"
    f"{guard_line}\n\n"
    f"未完成项目：\n{items}\n\n"
    "请继续处理 TODO.md 中的未完成项目。完成一项后将对应条目标记为 [x]，全部完成后再结束。"
)

print(json.dumps({"decision": "block", "reason": reason}))
PY
