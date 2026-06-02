#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
HOOK="${PROJECT_ROOT}/.claude/hooks/stop-todo-check.sh"
PROJECT_SETTINGS="${PROJECT_ROOT}/.claude/settings.json"
PROJECT_HOOKS="${PROJECT_ROOT}/.claude/hooks.json"

TMPDIR=$(mktemp -d)
cleanup() {
    rm -rf "${TMPDIR}"
}
trap cleanup EXIT

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

pass() {
    echo "[PASS] $*"
}

run_hook() {
    local cwd="$1"
    local active="$2"
    local out_file="$3"
    python3 - "${cwd}" "${active}" <<'PY' | "${HOOK}" > "${out_file}"
import json
import sys

cwd = sys.argv[1]
active = sys.argv[2].lower() == "true"
print(json.dumps({"cwd": cwd, "stop_hook_active": active}))
PY
}

assert_allow() {
    local name="$1"
    local cwd="$2"
    local active="${3:-false}"
    local out="${TMPDIR}/${name}.out"

    run_hook "${cwd}" "${active}" "${out}" || fail "${name}: hook exited non-zero"
    if [ -s "${out}" ]; then
        fail "${name}: expected allow with empty stdout, got: $(cat "${out}")"
    fi
    pass "${name}"
}

assert_block() {
    local name="$1"
    local cwd="$2"
    local active="${3:-false}"
    local expected_count="$4"
    local out="${TMPDIR}/${name}.out"

    run_hook "${cwd}" "${active}" "${out}" || fail "${name}: hook exited non-zero"
    python3 - "${out}" "${expected_count}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
expected_count = sys.argv[2]
raw = path.read_text(encoding="ascii")
payload = json.loads(raw)
if payload.get("decision") != "block":
    raise SystemExit(f"expected block decision, got {payload!r}")
reason = payload.get("reason", "")
if f"TODO.md 中仍有 {expected_count} 项未完成" not in reason:
    raise SystemExit(f"missing expected count in reason: {reason!r}")
if "未完成项目" not in reason:
    raise SystemExit(f"missing item list in reason: {reason!r}")
PY
    pass "${name}"
}

mkdir -p "${TMPDIR}/absent"
assert_allow "missing TODO.md allows stop" "${TMPDIR}/absent"

mkdir -p "${TMPDIR}/complete"
cat > "${TMPDIR}/complete/TODO.md" <<'EOF'
# TODO

stop_hook_prevent_infinite_loop: true

- [x] done
1. [X] also done
EOF
assert_allow "complete TODO.md allows stop" "${TMPDIR}/complete"

mkdir -p "${TMPDIR}/default_guard"
cat > "${TMPDIR}/default_guard/TODO.md" <<'EOF'
# TODO

- [ ] finish relation trim audit
1. [ ] run hook tests
- [x] existing finished item
EOF
assert_block "incomplete TODO.md blocks first stop" "${TMPDIR}/default_guard" false 2
assert_allow "default guard allows second stop" "${TMPDIR}/default_guard" true

mkdir -p "${TMPDIR}/strict"
cat > "${TMPDIR}/strict/TODO.md" <<'EOF'
# TODO

stop_hook_prevent_infinite_loop: false

- [ ] finish the user-controlled task
EOF
assert_block "disabled guard blocks second stop" "${TMPDIR}/strict" true 1

mkdir -p "${TMPDIR}/alias"
cat > "${TMPDIR}/alias/TODO.md" <<'EOF'
# TODO

STOP_HOOK_PREVENT_INFINITE_LOOP=false

* [ ] support env-style config aliases
EOF
assert_block "uppercase env-style config disables guard" "${TMPDIR}/alias" true 1

python3 - "${PROJECT_SETTINGS}" "${PROJECT_HOOKS}" "${HOOK}" <<'PY'
import json
import pathlib
import re
import sys

settings_path = pathlib.Path(sys.argv[1])
hooks_path = pathlib.Path(sys.argv[2])
hook_path = pathlib.Path(sys.argv[3])
portable_command = "${CLAUDE_PROJECT_DIR}/.claude/hooks/stop-todo-check.sh"
mac_users = "/" + "Users/"
windows_users = "C:" + "\\" + "Users" + "\\"
local_path_pattern = re.compile(
    "|".join(
        re.escape(part)
        for part in [
            mac_users,
            windows_users,
            "Desktop/" + "GitMy",
            mac_users + ("ad" + "min"),
        ]
    )
)

if not settings_path.is_file():
    raise SystemExit(".claude/settings.json is missing; Claude Code does not read .claude/hooks.json as project settings")

settings_raw = settings_path.read_text()
if local_path_pattern.search(settings_raw):
    raise SystemExit(".claude/settings.json contains a local machine path")

settings = json.loads(settings_raw)
stop_groups = settings.get("hooks", {}).get("Stop", [])
commands = [
    handler.get("command")
    for group in stop_groups
    for handler in group.get("hooks", [])
    if handler.get("type") == "command"
]

if portable_command not in commands:
    raise SystemExit("Stop hook command is not registered portably in .claude/settings.json")

def normalized_path(value: str) -> str:
    return value.replace("\\", "/").rstrip("/")

expected_hook = normalized_path(str(hook_path))
project_root = normalized_path(str(hook_path.parents[2]))
resolved = [
    normalized_path(command.replace("${CLAUDE_PROJECT_DIR}", project_root))
    for command in commands
    if isinstance(command, str)
]
if expected_hook not in resolved:
    raise SystemExit("Stop hook command does not resolve to the project hook script")

if hooks_path.is_file():
    hooks_raw = hooks_path.read_text()
    if local_path_pattern.search(hooks_raw):
        raise SystemExit(".claude/hooks.json contains a local machine path")
    hooks_config = json.loads(hooks_raw)
    legacy_stop_commands = [
        handler.get("command")
        for group in hooks_config.get("hooks", [])
        if group.get("matcher", {}).get("event") == "Stop"
        for handler in group.get("hooks", [])
        if handler.get("type") == "command"
    ]
    if portable_command not in legacy_stop_commands:
        raise SystemExit("Stop hook command is not registered portably in .claude/hooks.json")
PY
pass "Claude project hook configs register Stop hook portably"

python3 - "${PROJECT_ROOT}" <<'PY'
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
patterns = [
    ("/" + "Users/" + ("may" + "iding"), "local macOS user path"),
    ("/" + "Users/" + ("ad" + "min"), "local macOS user path"),
    ("Desktop/" + "GitMy", "local desktop workspace path"),
    ("Documents/" + "GNFS", "local documents workspace path"),
    ("/home/" + "runner", "GitHub runner workspace path"),
    ("D:" + "/a/", "GitHub Windows runner workspace path"),
    ("D:" + "\\a\\", "GitHub Windows runner workspace path"),
    ("may" + "iding", "local username"),
]

try:
    tracked_raw = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "-z"],
        stderr=subprocess.DEVNULL,
    )
except (OSError, subprocess.CalledProcessError) as exc:
    raise SystemExit(f"cannot list tracked files for local path scan: {exc}") from exc

leaks: list[str] = []
for raw_name in tracked_raw.split(b"\0"):
    if not raw_name:
        continue
    rel = raw_name.decode("utf-8", errors="replace")
    path = root / rel
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        continue
    for line_no, line in enumerate(text.splitlines(), 1):
        for needle, label in patterns:
            if needle in line:
                leaks.append(f"{rel}:{line_no}: {label}: {line[:160]}")

if leaks:
    shown = "\n".join(leaks[:40])
    extra = "" if len(leaks) <= 40 else f"\n... {len(leaks) - 40} more"
    raise SystemExit(f"tracked files contain non-portable local paths:\n{shown}{extra}")
PY
pass "tracked files do not contain known local machine paths"

echo "Stop TODO hook tests passed."
