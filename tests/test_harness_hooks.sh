#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
HOOK="${PROJECT_ROOT}/.claude/hooks/project-guard.py"
CHECKER="${PROJECT_ROOT}/scripts/check_harness.py"
PYTHON=${PYTHON:-python3}
ZSH_EXECUTABLE="${GNFS_ZSH_EXECUTABLE:-}"
if [[ -z "${ZSH_EXECUTABLE}" ]]; then
    ZSH_EXECUTABLE="$(command -v zsh || true)"
fi
if [[ -n "${ZSH_EXECUTABLE}" && ! -x "${ZSH_EXECUTABLE}" ]]; then
    ZSH_EXECUTABLE=""
fi

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

assert_runner_argument_error() {
    local name="$1"
    local option="$2"
    shift 2
    local output runner_status

    set +e
    output=$("${ZSH_EXECUTABLE}" "${PROJECT_ROOT}/scripts/test.sh" --no-build "$@" 2>&1)
    runner_status=$?
    set -e
    if [[ "${runner_status}" -ne 2 || "${output}" != *"参数错误"* ||
          "${output}" != *"${option}"* ]]; then
        fail "${name}: expected parser status 2 for ${option}, got status=${runner_status}: ${output}"
    fi
    if [[ "${output}" == *"冒烟测试"* || "${output}" == *"编译"* ]]; then
        fail "${name}: parser error reached a build/test mode"
    fi
    pass "${name}"
}

assert_runner_report_cleanup() {
    local name="$1"
    local mode="$2"
    local runner_build_dir="${TEST_TMPDIR}/${name// /_}/build"
    local output="${TEST_TMPDIR}/${name// /_}.runner.out"
    mkdir -p "${runner_build_dir}"

    if ! GNFS_BUILD_DIR="${runner_build_dir}" \
        "${PROJECT_ROOT}/scripts/test.sh" --no-build --no-color "${mode}" \
        >"${output}" 2>&1; then
        fail "${name}: runner should finish its report path for a missing binary"
    fi
    if ! grep -q '总计: 1' "${output}" || ! grep -q '跳过: 1' "${output}"; then
        fail "${name}: summary did not include the skipped test: $(<"${output}")"
    fi
    "${PYTHON}" - "${runner_build_dir}/test_report.json" "${name}" <<'PY'
import json
import pathlib
import sys

report_path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
try:
    report = json.loads(report_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"{name}: missing or invalid test report: {exc}")
if report.get("total") != 1 or report.get("passed") != 0 or \
        report.get("failed") != 0 or report.get("skipped") != 1:
    raise SystemExit(f"{name}: unexpected report counters: {report!r}")
tests = report.get("tests")
if not isinstance(tests, list) or len(tests) != 1 or \
        tests[0].get("status") != "skip":
    raise SystemExit(f"{name}: unexpected report entries: {report!r}")
PY
    pass "${name}"
}

assert_runner_failure_report() {
    local name="$1"
    local mode="$2"
    local runner_build_dir="${TEST_TMPDIR}/${name// /_}/build"
    local output="${TEST_TMPDIR}/${name// /_}.runner.out"
    local false_binary
    false_binary=$(type -P false) || fail "${name}: false executable is unavailable"
    mkdir -p "${runner_build_dir}"
    ln -s "${false_binary}" "${runner_build_dir}/test_stress"

    local runner_exit=0
    GNFS_BUILD_DIR="${runner_build_dir}" \
        "${PROJECT_ROOT}/scripts/test.sh" --no-build --no-color "${mode}" \
        >"${output}" 2>&1 || runner_exit=$?
    if [[ "${runner_exit}" == 0 ]]; then
        fail "${name}: runner must preserve a real test failure"
    fi
    if ! grep -q '总计: 1' "${output}" || ! grep -q '失败: 1' "${output}"; then
        fail "${name}: failure summary was lost: $(<"${output}")"
    fi
    "${PYTHON}" - "${runner_build_dir}/test_report.json" "${name}" <<'PY'
import json
import pathlib
import sys

report_path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
try:
    report = json.loads(report_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"{name}: missing or invalid test report: {exc}")
if report.get("total") != 1 or report.get("passed") != 0 or \
        report.get("failed") != 1 or report.get("skipped") != 0:
    raise SystemExit(f"{name}: unexpected report counters: {report!r}")
tests = report.get("tests")
if not isinstance(tests, list) or len(tests) != 1 or \
        tests[0].get("status") != "fail":
    raise SystemExit(f"{name}: unexpected report entries: {report!r}")
PY
    pass "${name}"
}

assert_unknown_module_rejected() {
    local name="$1"
    local runner_root="${TEST_TMPDIR}/${name// /_}/repo"
    local output="${TEST_TMPDIR}/${name// /_}.runner.out"
    local runner_status=0
    local unknown_module='__gnfs_unknown_module__"'

    # Run a copied runner so its report and any skipped-build bookkeeping stay
    # inside TEST_TMPDIR instead of touching the checkout's build directory.
    mkdir -p "${runner_root}/scripts/lib" "${runner_root}/build"
    cp "${PROJECT_ROOT}/scripts/test.sh" "${runner_root}/scripts/test.sh"
    cp "${PROJECT_ROOT}/scripts/lib/process_tree_timeout.zsh" \
        "${runner_root}/scripts/lib/process_tree_timeout.zsh"

    set +e
    zsh "${runner_root}/scripts/test.sh" --no-build --no-color module \
        "${unknown_module}" >"${output}" 2>&1
    runner_status=$?
    set -e
    if [[ "${runner_status}" -eq 0 ]]; then
        fail "${name}: unknown module selection unexpectedly succeeded: $(<"${output}")"
    fi
    if ! grep -q '未知模块' "${output}" || ! grep -q '失败: 1' "${output}"; then
        fail "${name}: missing fail-closed module diagnostic: $(<"${output}")"
    fi
    "${PYTHON}" - "${runner_root}/build/test_report.json" "${name}" <<'PY'
import json
import pathlib
import sys

report_path = pathlib.Path(sys.argv[1])
name = sys.argv[2]
try:
    report = json.loads(report_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"{name}: missing or invalid test report: {exc}")
if report.get("total") != 1 or report.get("passed") != 0 or \
        report.get("failed") != 1 or report.get("skipped") != 0:
    raise SystemExit(f"{name}: unexpected report counters: {report!r}")
tests = report.get("tests")
if not isinstance(tests, list) or len(tests) != 1 or \
        tests[0].get("status") != "fail" or \
        tests[0].get("detail") != "unknown_module":
    raise SystemExit(f"{name}: unexpected report entry: {report!r}")
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

if [[ -n "${ZSH_EXECUTABLE}" ]]; then
    assert_runner_argument_error "missing timeout value is rejected" "--timeout" --timeout
    assert_runner_argument_error "negative timeout value is rejected" "--timeout" --timeout -1
    assert_runner_argument_error "invalid timeout value is rejected" "--timeout" --timeout 0
    assert_runner_argument_error "oversized timeout value is rejected" "--timeout" --timeout 9223372036855
    assert_runner_argument_error "invalid parallelism value is rejected" "-j" -j 0
    assert_runner_argument_error "negative parallelism value is rejected" "-j" -j -1
    assert_runner_argument_error "oversized parallelism value is rejected" "-j" -j 9223372036854775808
    assert_runner_argument_error "missing parallelism value is rejected" "-j" -j
    assert_runner_argument_error "missing build type is rejected" "-t" -t
    assert_runner_argument_error "missing retry value is rejected" "--retry" --retry
    assert_runner_argument_error "negative retry value is rejected" "--retry" --retry -1
    assert_runner_argument_error "invalid retry value is rejected" "--retry" --retry nope
else
    echo "[SKIP] zsh-specific runner argument contracts (zsh is unavailable)"
fi

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

if command -v zsh >/dev/null 2>&1; then
    assert_runner_report_cleanup "perf missing binary report" perf
    assert_runner_report_cleanup "stress missing binary report" stress
    assert_runner_failure_report "stress real failure report" stress
    assert_unknown_module_rejected "unknown module selection fails closed"
else
    pass "zsh-only Runner contracts skipped"
fi

echo "GNFS Harness Hook tests passed."
