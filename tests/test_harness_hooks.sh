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
    cp "${PROJECT_ROOT}/scripts/lib/changed_files.zsh" \
        "${runner_root}/scripts/lib/changed_files.zsh"

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

assert_runner_target_builds() {
    "${PYTHON}" - "${PROJECT_ROOT}" "${TEST_TMPDIR}/targeted runner" "${ZSH_EXECUTABLE}" <<'PYTEST'
import json
import os
from pathlib import Path
import subprocess
import sys

project, fixture, zsh = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
fixture.mkdir()
bin_dir = fixture / "bin"
bin_dir.mkdir()
log = fixture / "calls.jsonl"
common = "import json, os, pathlib, subprocess, sys\n"
common += "with open(os.environ['GNFS_STUB_LOG'], 'a') as f:\n"
common += "    f.write(json.dumps([pathlib.Path(sys.argv[0]).name, *sys.argv[1:]]) + '\\n')\n"

def executable(path, body):
    path.write_text("#!" + sys.executable + "\n" + common + body)
    path.chmod(0o755)

executable(bin_dir / "cmake", """
args = sys.argv[1:]
if '--build' not in args:
    build = pathlib.Path(args[args.index('-B') + 1])
    build.mkdir(parents=True, exist_ok=True)
    build_type = next(a.split('=', 1)[1] for a in args if a.startswith('-DCMAKE_BUILD_TYPE='))
    (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=' + build_type + '\\n')
raise SystemExit(int(os.environ.get('GNFS_STUB_BUILD_EXIT', '0')) if '--build' in args else 0)
""")
env = os.environ.copy()
env.update(PATH=str(bin_dir) + os.pathsep + env['PATH'], GNFS_STUB_LOG=str(log))
env.pop('GNFS_TEST_PROCESS_SUPERVISOR', None)
env.pop('GNFS_TSAN_BUILD_DIR', None)

case_index = 0

def invoke(options, *, cached=False, build_exit=0, missing_binary=False,
           relative_build=False, tsan=False):
    global case_index
    case_index += 1
    build = fixture / str(case_index) / "build with spaces"
    build.mkdir(parents=True)
    caller = fixture / str(case_index) / "caller directory"
    caller.mkdir()
    build_value = os.path.relpath(build, caller) if relative_build else str(build)
    run_env = {**env, 'GNFS_BUILD_DIR': build_value, 'GNFS_STUB_BUILD_EXIT': str(build_exit)}
    if tsan:
        run_env['GNFS_BUILD_DIR'] = str(fixture / 'unused normal build')
        run_env['GNFS_TSAN_BUILD_DIR'] = build_value
    if cached:
        (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\n')
    if not missing_binary:
        executable(build / 'test_integer', "raise SystemExit(0)\n")
    executable(build / 'gnfs_test_process_supervisor',
               "args = sys.argv[sys.argv.index('--') + 1:]\nraise SystemExit(subprocess.call(args))\n")
    log.write_text('')
    result = subprocess.run([zsh, str(project / 'scripts/test.sh'), '--no-color', *options],
                            env=run_env, cwd=caller,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=10)
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    return result, calls, build


def require(condition, detail):
    if not condition:
        raise SystemExit(detail)

for options, cached in [(['run', 'integer', '--suite', 'value with spaces'], False),
                        (['-v', 'run', 'test_integer'], True)]:
    result, calls, build = invoke(options, cached=cached)
    require(result.returncode == 0, result.stdout)
    builds = [c for c in calls if c[:2] == ['cmake', '--build']]
    require(len(builds) == 1, f'expected one build: {calls!r}')
    command = builds[0]
    if '-v' in options:
        require('--verbose' in command and '--' not in command,
                f'verbose build depends on generator-specific flags: {command!r}')
    require('--target' in command, f'single-test run requested a full build: {command!r}')
    targets = command[command.index('--target') + 1:]
    if '--' in targets:
        targets = targets[:targets.index('--')]
    require(targets == ['test_integer', 'gnfs_test_process_supervisor'],
            f'unexpected build targets: {targets!r}')
    configurations = [c for c in calls if c[0] == 'cmake' and '--build' not in c]
    require(len(configurations) == (0 if cached else 1), f'configuration mismatch: {calls!r}')
    tests = [c for c in calls if c[0] == 'test_integer']
    expected_args = ['--suite', 'value with spaces'] if not cached else []
    require(tests == [['test_integer', *expected_args]], f'test arguments changed: {calls!r}')
    report = json.loads((build / 'test_report.json').read_text())
    require(report['passed'] == 1 and report['failed'] == 0 and report['skipped'] == 0,
            f'single-test run did not execute: {report!r}')

for name in ['does_not_exist', '../test_integer', 'integer*', 'integer"']:
    result, calls, _ = invoke(['run', name])
    require(result.returncode != 0 and not calls,
            f'invalid test reached a command: {name!r}, {calls!r}, {result.stdout}')

result, calls, _ = invoke(['--no-build', 'run', 'integer'])
require(result.returncode == 0 and not any(c[0] == 'cmake' for c in calls),
        f'--no-build invoked CMake: {calls!r}, {result.stdout}')
require(any(c[0] == 'test_integer' for c in calls), '--no-build did not execute the test')

result, calls, _ = invoke(['run', 'integer'], build_exit=41)
require(result.returncode != 0 and not any(c[0] == 'test_integer' for c in calls),
        f'failed build continued into tests: {calls!r}, {result.stdout}')

for options in [['run', 'integer'], ['--no-build', 'run', 'integer']]:
    result, calls, build = invoke(options, missing_binary=True)
    require(result.returncode != 0, f'missing explicitly requested binary succeeded: {result.stdout}')
    report = json.loads((build / 'test_report.json').read_text())
    require(report['total'] == 1 and report['failed'] == 1 and report['skipped'] == 0 and
            report['tests'][0]['detail'] == 'binary_missing',
            f'missing requested test was not reported as a failure: {report!r}')

result, calls, build = invoke(['run', 'integer'], relative_build=True)
require(result.returncode == 0, f'relative build failed: {result.stdout}')
for command in (c for c in calls if c[0] == 'cmake'):
    selector = '--build' if '--build' in command else '-B'
    require(command[command.index(selector) + 1] == str(build.resolve()),
            f'CMake did not receive the caller-relative absolute path: {command!r}')
supervisors = [c for c in calls if c[0] == 'gnfs_test_process_supervisor']
require(len(supervisors) == 1 and
        supervisors[0][supervisors[0].index('--') + 1] == str(build.resolve() / 'test_integer'),
        f'supervisor did not receive an absolute test path: {calls!r}')
report = json.loads((build / 'test_report.json').read_text())
require(report['passed'] == 1 and report['skipped'] == 0,
        f'relative build report was not written beside the binaries: {report!r}')

# Exercise TSAN directory selection on every host without compiling or launching
# instrumented tests. The fake build fails after recording its command paths.
executable(bin_dir / 'uname', "print('Linux')\n")
result, calls, build = invoke(['tsan-relation'], relative_build=True, tsan=True, build_exit=41)
require(result.returncode != 0, 'synthetic TSAN build failure was ignored')
cmake_calls = [c for c in calls if c[0] == 'cmake']
require(len(cmake_calls) == 2, f'TSAN configuration/build were not selected: {calls!r}')
for command in cmake_calls:
    selector = '--build' if '--build' in command else '-B'
    require(command[command.index(selector) + 1] == str(build.resolve()),
            f'TSAN override was not normalized: {command!r}')
require('-DGNFS_ENABLE_TSAN=ON' in cmake_calls[0], 'TSAN instrumentation option was lost')

result, calls, _ = invoke(['build'], cached=True)
require(result.returncode == 0 and not any('--target' in c for c in calls),
        f'build mode no longer builds the full catalog: {calls!r}, {result.stdout}')
PYTEST
    pass "single-test builds select only their target and supervisor"
}

assert_runner_required_binaries() {
    "${PYTHON}" - "${PROJECT_ROOT}" "${TEST_TMPDIR}/required runner" "${ZSH_EXECUTABLE}" <<'PYTEST'
from concurrent.futures import ThreadPoolExecutor
from threading import Lock
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

project, fixture, zsh = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
(fixture / 'scripts/lib').mkdir(parents=True)
for name in ['test.sh', 'lib/changed_files.zsh']:
    shutil.copy2(project / 'scripts' / name, fixture / 'scripts' / name)
# Process supervision has separate contracts. This fixture supplies child
# results directly and exercises the real selection, aggregation, and reports.
(fixture / 'scripts/lib/process_tree_timeout.zsh').write_text(
    'run_with_timeout() {\n    shift\n    RUN_OUTPUT=""\n    "$@"\n}\n')
runner = (project / 'scripts/test.sh').read_text()
optional_match = re.search(r'WINDOWS_UNAVAILABLE_TESTS=\(\n(.*?)\n\)', runner, re.S)
optional_targets = optional_match.group(1).split() if optional_match else []
# Exercise the real aggregation logic with small synthetic catalogs. Running
# every production placeholder would only duplicate the same launch contract.
fixture_runner = re.sub(r'(?ms)^SMOKE_TESTS=\(\n.*?^\)',
                        'SMOKE_TESTS=(\n    test_integer\n    test_fb_checkpoint\n)', runner, count=1)
fixture_sieve = ' '.join(['test_sieve_basic', *optional_targets])
fixture_runner = re.sub(r'(?m)^    sieve\s+"[^"]*"',
                        '    sieve "' + fixture_sieve + '"', fixture_runner, count=1)
(fixture / 'scripts/test.sh').write_text(fixture_runner)
(fixture / 'src/factor_base').mkdir(parents=True)
source = fixture / 'src/factor_base/builder.cpp'
source.write_text('// baseline\n')
(fixture / '.gitignore').write_text('.cache/\n')
for args in [['init', '-q'], ['add', '.'],
             ['-c', 'user.name=Runner Fixture', '-c', 'user.email=fixture@example.invalid',
              '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture baseline']]:
    subprocess.run(['git', *args], cwd=fixture, check=True, stdout=subprocess.DEVNULL)
base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=fixture, text=True).strip()
source.write_text('// changed\n')
all_tests = ['test_factor_base', 'test_fb_checkpoint', 'test_fb_roots_parallel',
             'test_integer', 'test_regression_gate', 'test_sieve_basic', *optional_targets]
cache = fixture / '.cache'
bin_dir = cache / 'bin'
bin_dir.mkdir(parents=True)

def executable(path, body, interpreter='/bin/sh'):
    path.write_text('#!' + interpreter + '\n' + body)
    path.chmod(0o755)

executable(bin_dir / 'cmake', 'exit 0\n')
executable(bin_dir / 'ctest', 'exit 0\n')
# Timing is outside this selection contract; avoid hundreds of Python startups.
executable(bin_dir / 'clock', 'printf \"0\\n\"\n')
executable(bin_dir / 'uname', 'printf "%s\\n" "$GNFS_STUB_HOST"\n')
case_index = 0
case_lock = Lock()

def invoke(options, *, missing=(), host='Linux', win32='0', exit_77=(), exit_codes=None,
           stale_report=False, empty=False, nonexecutable=None):
    global case_index
    with case_lock:
        case_index += 1
        case_id = case_index
    build = cache / str(case_id)
    build.mkdir()
    (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\nGNFS_BUILD_TESTS:BOOL=OFF\n')
    if win32 is not None:
        (build / 'gnfs-test-platform.txt').write_text('WIN32=' + win32 + '\n')
    if stale_report:
        (build / 'test_report.json').write_text('{"stale_previous_pass": true}\n')
    if not empty:
        for name in all_tests:
            if name not in missing:
                code = (exit_codes or {}).get(name, 77 if name in exit_77 else 0)
                executable(build / name,
                           'printf "GNFS_FIXTURE_EXECUTED %s\\n" ' + name + '\nexit ' + str(code) + '\n')
    if nonexecutable:
        (build / nonexecutable).chmod(0o644)
    env = {**os.environ, 'PATH': str(bin_dir) + os.pathsep + os.environ['PATH'],
           'GNFS_BUILD_DIR': str(build), 'GNFS_TEST_BASE_SHA': base, 'GNFS_STUB_HOST': host,
           'GNFS_PYTHON_EXECUTABLE': str(bin_dir / 'clock')}
    env.pop('GNFS_TEST_PROCESS_SUPERVISOR', None)
    result = subprocess.run([zsh, str(fixture / 'scripts/test.sh'), '--no-color', *options],
                            cwd=fixture, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    report = json.loads((build / 'test_report.json').read_text())
    return result, report

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def failed_missing(options, name, **kwargs):
    result, report = invoke(options, **kwargs)
    entries = [entry for entry in report['tests'] if entry['name'] == name]
    require(result.returncode != 0 and report['failed'] == 1 and len(entries) == 1 and
            entries[0]['status'] == 'fail' and entries[0]['detail'] == 'binary_missing',
            f'required binary was not a failure: {options!r}, {report!r}, {result.stdout}')
    require('合并门禁全部通过' not in result.stdout, 'failed gate claimed complete success')

def check_fail_fast():
    for options, name, code in [(['gate'], 'test_integer', 1),
                                 (['gate'], 'test_integer', 124),
                                 (['run', 'test_integer'], 'test_integer', 1),
                                 (['module', 'factor_base'], 'test_factor_base', 124)]:
        result, report = invoke(['--no-build', '--fail-fast', *options],
                                exit_codes={name: code}, stale_report=True)
        expected = 'timeout' if code == 124 else 'fail'
        require(result.returncode == 1 and report.get('total') == 1 and report['failed'] == 1 and
                report['passed'] == 0 and report['skipped'] == 0 and
                report['tests'][0]['name'] == name and report['tests'][0]['status'] == expected,
                f'fail-fast lost its report or ran later tests: {report!r}, {result.stdout}')

    for options, kwargs in [(['module', 'factor_base'], {'missing': {'test_fb_checkpoint'}}),
                            (['changed'], {'nonexecutable': 'test_fb_checkpoint'})]:
        result, report = invoke(['--no-build', '--fail-fast', *options], stale_report=True, **kwargs)
        executed = [line.removeprefix('GNFS_FIXTURE_EXECUTED ') for line in result.stdout.splitlines()
                    if line.startswith('GNFS_FIXTURE_EXECUTED ')]
        require(result.returncode == 1 and report.get('total') == 2 and report['passed'] == 1 and
                report['failed'] == 1 and report['skipped'] == 0 and
                report['tests'][-1]['detail'] == 'binary_missing' and executed == ['test_factor_base'],
                f'fail-fast missing binary ran later children: {report!r}, {result.stdout}')


def check_missing_selections():
    result, report = invoke(['--no-build', 'module', 'factor_base'], empty=True)
    require(result.returncode != 0 and report['failed'] == 3 and report['skipped'] == 0,
            f'empty required module succeeded: {report!r}, {result.stdout}')
    result, report = invoke(['--no-build', 'module', *(['factor_base'] * 256)], empty=True)
    require(result.returncode == 1 and report['failed'] == 768 and report['skipped'] == 0,
            f'failure count wrapped to a successful exit: {report!r}')
    for options in [['module', 'factor_base'], ['--no-build', 'mod', 'factor_base'],
                    ['--no-build', 'changed'], ['diff']]:
        failed_missing(options, 'test_fb_checkpoint', missing={'test_fb_checkpoint'})
    failed_missing(['--no-build', 'module', 'factor_base'], 'test_fb_checkpoint',
                   nonexecutable='test_fb_checkpoint')
    failed_missing(['--no-build', 'gate'], 'test_regression_gate', missing={'test_regression_gate'})
    failed_missing(['gate', '--quick'], 'test_integer', missing={'test_integer'})

def check_successful_selections():
    for options in [['module', 'factor_base'], ['--no-build', 'changed']]:
        result, report = invoke(options)
        require(result.returncode == 0 and report['passed'] == 3 and report['failed'] == 0 and
                report['skipped'] == 0 and {entry['name'] for entry in report['tests']} ==
                {'test_factor_base', 'test_fb_checkpoint', 'test_fb_roots_parallel'},
                f'complete selection did not pass: {report!r}')
    result, report = invoke(['--no-build', 'module', 'factor_base'], exit_77={'test_fb_checkpoint'})
    require(result.returncode == 0 and report['passed'] == 2 and report['failed'] == 0 and
            report['skipped'] == 1 and any(entry['detail'] == 'exit_code=77' for entry in report['tests']),
            f'platform exit 77 stopped being a skip: {report!r}')

# This allowlist follows CMake's target availability, not whether any arbitrary
# build cache disabled all tests. Guard the deliberate cross-file contract.
windows_optional = set(re.search(r'WINDOWS_UNAVAILABLE_TESTS=\(\n(.*?)\n\)', runner, re.S).group(1).split())
cmake_optional = set()
conditions = []
for match in re.finditer(r'(?im)^\s*(if|elseif|else|endif|add_executable)\s*\(([^)]*)\)',
                         (project / 'CMakeLists.txt').read_text()):
    command, args = match.groups()
    command, args = command.lower(), ' '.join(args.split())
    if command == 'if':
        conditions.append(args)
    elif command in {'elseif', 'else'}:
        conditions[-1] = command + ' ' + args
    elif command == 'endif':
        conditions.pop()
    elif args.startswith('test_') and 'NOT WIN32' in conditions:
        cmake_optional.add(args.split()[0])
require(windows_optional == cmake_optional and windows_optional,
        f'Windows exclusions differ from CMake: {windows_optional ^ cmake_optional!r}')
def check_target_platforms():
    result, report = invoke(['--no-build', 'gate'], exit_77={'test_integer'})
    require(result.returncode == 0 and report['failed'] == 0 and report['skipped'] == 1,
            f'complete gate or exit-77 skip regressed: {report!r}')
    for host in ['MINGW64_NT-10.0', 'MSYS_NT-10.0']:
        result, report = invoke(['--no-build', 'module', 'sieve'], missing=windows_optional, host=host, win32='1')
        skipped = {entry['name'] for entry in report['tests'] if entry['status'] == 'skip'}
        require(result.returncode == 0 and report['failed'] == 0 and skipped == windows_optional,
                f'Windows conditional targets became mandatory: {report!r}')
        require(all(entry['detail'] == 'platform_unsupported' for entry in report['tests']
                    if entry['status'] == 'skip'), 'conditional skip lacks a platform reason')
        failed_missing(['--no-build', 'module', 'sieve'], 'test_sieve_basic',
                       missing=windows_optional | {'test_sieve_basic'}, host=host, win32='1')
    for host, win32 in [('Linux', '0'), ('Darwin', '0'), ('CYGWIN_NT-10.0', '0'),
                        ('MSYS_NT-10.0', '0'), ('MINGW64_NT-10.0', None), ('MSYS_NT-10.0', 'invalid')]:
        result, report = invoke(['--no-build', 'module', 'sieve'], missing=windows_optional,
                                host=host, win32=win32)
        failed = {entry['name'] for entry in report['tests'] if entry['status'] == 'fail'}
        require(result.returncode == 1 and failed == windows_optional and report['skipped'] == 0,
                f'Windows exclusions leaked to {host}: {report!r}')

# Every case owns its build/report directory; the runner fixture stays read-only
# until these independent groups finish. Bound concurrency to keep the existing
# ten-second HarnessHooks budget without weakening its coverage.
with ThreadPoolExecutor(max_workers=3) as executor:
    futures = [executor.submit(check) for check in
               [check_fail_fast, check_missing_selections, check_successful_selections,
                check_target_platforms]]
    for future in futures:
        future.result()

# An unexpected gate error must still produce a failed report even if the
# function did not record a failed test before returning.
runner_path = fixture / 'scripts/test.sh'
modified, count = re.subn(r'(?ms)^do_gate\(\)\s*\{\n.*?^\}',
                          'do_gate() {\n    return 41\n}', runner_path.read_text(), count=1)
require(count == 1, 'gate fixture injection did not find its function')
runner_path.write_text(modified)
result, report = invoke(['--no-build', 'gate'])
require(result.returncode == 1 and report['failed'] == 1 and report['tests'][0]['name'] ==
        'gate_execution' and report['tests'][0]['detail'] == 'exit_code=41',
        f'unrecorded gate failure was ignored: {report!r}, {result.stdout}')
PYTEST
    pass "required selections fail for missing binaries and preserve platform skips"
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
    assert_runner_target_builds
    assert_runner_required_binaries
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
