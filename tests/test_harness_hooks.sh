#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
HOOK="${PROJECT_ROOT}/.claude/hooks/project-guard.py"
CHECKER="${PROJECT_ROOT}/scripts/check_harness.py"
PYTHON=${PYTHON:-python3}

TEST_TMPDIR=$(mktemp -d)
cleanup() {
    rm -rf "${TEST_TMPDIR}"
}
trap cleanup EXIT

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

pass() {
    echo "[PASS] $*"
}

invoke_hook() {
    local root="$1"
    local payload="$2"
    local output="$3"
    printf '%s\n' "${payload}" | CLAUDE_PROJECT_DIR="${root}" "${PYTHON}" "${HOOK}" > "${output}"
}

assert_empty() {
    local name="$1"
    local root="$2"
    local payload="$3"
    local output="${TEST_TMPDIR}/${name// /_}.json"

    invoke_hook "${root}" "${payload}" "${output}"
    if [[ -s "${output}" ]]; then
        fail "${name}: expected no hook decision, got $(<"${output}")"
    fi
    pass "${name}"
}

assert_write_denied() {
    local name="$1"
    local file_path="$2"
    local output="${TEST_TMPDIR}/${name// /_}.json"
    local payload
    payload=$("${PYTHON}" - "${file_path}" <<'PY'
import json
import sys

print(json.dumps({
    "hook_event_name": "PreToolUse",
    "tool_name": "Edit",
    "tool_input": {"file_path": sys.argv[1]},
}))
PY
)

    invoke_hook "${PROJECT_ROOT}" "${payload}" "${output}"
    "${PYTHON}" - "${output}" <<'PY'
import json
import pathlib
import sys

payload = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
specific = payload.get("hookSpecificOutput", {})
if specific.get("hookEventName") != "PreToolUse":
    raise SystemExit(f"missing PreToolUse output: {payload!r}")
if specific.get("permissionDecision") != "deny":
    raise SystemExit(f"expected deny: {payload!r}")
if "generated" not in specific.get("permissionDecisionReason", "") and "worktree" not in specific.get("permissionDecisionReason", ""):
    raise SystemExit(f"missing generated/worktree reason: {payload!r}")
PY
    pass "${name}"
}

assert_stop_blocked() {
    local name="$1"
    local root="$2"
    local expected="$3"
    local output="${TEST_TMPDIR}/${name// /_}.json"

    invoke_hook "${root}" '{"hook_event_name":"Stop","stop_hook_active":false}' "${output}"
    "${PYTHON}" - "${output}" "${expected}" <<'PY'
import json
import pathlib
import sys

payload = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
if payload.get("decision") != "block":
    raise SystemExit(f"expected Stop block: {payload!r}")
if sys.argv[2] not in payload.get("reason", ""):
    raise SystemExit(f"missing reason fragment {sys.argv[2]!r}: {payload!r}")
PY
    pass "${name}"
}

"${PYTHON}" "${CHECKER}" >/dev/null
pass "repository Harness checker passes"

assert_write_denied "root build output is denied" "${PROJECT_ROOT}/build/CMakeCache.txt"
assert_write_denied "named build output is denied" "${PROJECT_ROOT}/build-release/result.txt"
assert_write_denied "CMakeFiles output is denied" "${PROJECT_ROOT}/tmp/CMakeFiles/rules.ninja"
assert_write_denied "Claude worktree is denied" "${PROJECT_ROOT}/.claude/worktrees/agent/file.cpp"
assert_write_denied "project worktree is denied" "${PROJECT_ROOT}/.worktrees/feature/file.cpp"

assert_empty \
    "build-test skill remains editable" \
    "${PROJECT_ROOT}" \
    "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${PROJECT_ROOT}/.agents/skills/build-test/SKILL.md\"}}"
assert_empty \
    "project scripts remain editable" \
    "${PROJECT_ROOT}" \
    "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${PROJECT_ROOT}/scripts/test.sh\"}}"
assert_empty "malformed input fails open" "${PROJECT_ROOT}" 'not-json'

mkdir -p "${TEST_TMPDIR}/passing/scripts" "${TEST_TMPDIR}/failing/scripts" "${TEST_TMPDIR}/missing"
printf '%s\n' 'raise SystemExit(0)' > "${TEST_TMPDIR}/passing/scripts/check_harness.py"
printf '%s\n' 'import sys' 'print("synthetic harness failure", file=sys.stderr)' 'raise SystemExit(1)' \
    > "${TEST_TMPDIR}/failing/scripts/check_harness.py"

assert_empty \
    "passing Stop check allows completion" \
    "${TEST_TMPDIR}/passing" \
    '{"hook_event_name":"Stop","stop_hook_active":false}'
assert_stop_blocked "failing Stop check blocks completion" "${TEST_TMPDIR}/failing" "synthetic harness failure"
assert_stop_blocked "missing checker blocks completion" "${TEST_TMPDIR}/missing" "checker is missing"
assert_empty \
    "active Stop hook prevents loops" \
    "${TEST_TMPDIR}/failing" \
    '{"hook_event_name":"Stop","stop_hook_active":true}'

echo "GNFS Harness Hook tests passed."
