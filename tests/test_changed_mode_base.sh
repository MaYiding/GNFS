#!/usr/bin/env zsh

set -euo pipefail
unsetopt BG_NICE

SCRIPT_DIR="${0:A:h}"
PROJECT_ROOT="${SCRIPT_DIR:h}"
source "${PROJECT_ROOT}/scripts/lib/changed_files.zsh"

TEST_TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/gnfs-changed-base.XXXXXX")
cleanup() {
    rm -rf -- "$TEST_TMPDIR"
}
trap cleanup EXIT

fail() {
    print -u2 -r -- "[FAIL] $*"
    exit 1
}

pass() {
    print -r -- "[PASS] $*"
}

repo="${TEST_TMPDIR}/repo"
remote="${TEST_TMPDIR}/remote.git"
git init --quiet --initial-branch=main -- "$repo"
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name GNFS
print -r -- base >"${repo}/base.txt"
git -C "$repo" add base.txt
git -C "$repo" commit --quiet -m base
base_sha=$(git -C "$repo" rev-parse HEAD)

git init --quiet --bare -- "$remote"
git -C "$repo" remote add origin "$remote"
git -C "$repo" push --quiet --set-upstream origin main

# Advance origin/main after creating the feature point. The default resolver
# must choose the common ancestor, not the newer remote tip.
git -C "$repo" branch feature
print -r -- main-only >"${repo}/main.txt"
git -C "$repo" add main.txt
git -C "$repo" commit --quiet -m main-only
git -C "$repo" push --quiet origin main
git -C "$repo" fetch --quiet origin main
main_tip=$(git -C "$repo" rev-parse origin/main)
git -C "$repo" switch --quiet feature
print -r -- feature >"${repo}/feature.txt"
git -C "$repo" add feature.txt
git -C "$repo" commit --quiet -m feature
feature_tip=$(git -C "$repo" rev-parse HEAD)

unset GNFS_TEST_BASE_SHA BASE_SHA GITHUB_BASE_REF
default_base=$(resolve_changed_base "$repo")
[[ "$default_base" == "$base_sha" ]] ||
    fail "default base should be merge-base ${base_sha}, got ${default_base}"
[[ "$main_tip" != "$base_sha" && "$feature_tip" != "$base_sha" ]] ||
    fail "fixture did not create divergent branch tips"
pass "default origin/main merge-base"

GNFS_TEST_BASE_SHA="$base_sha" BASE_SHA="$main_tip" GITHUB_BASE_REF=missing
explicit_base=$(resolve_changed_base "$repo")
[[ "$explicit_base" == "$base_sha" ]] ||
    fail "GNFS_TEST_BASE_SHA must take precedence, got ${explicit_base}"
pass "explicit GNFS_TEST_BASE_SHA precedence"

unset GNFS_TEST_BASE_SHA
BASE_SHA="$base_sha" GITHUB_BASE_REF=missing
ci_base=$(resolve_changed_base "$repo")
[[ "$ci_base" == "$base_sha" ]] ||
    fail "BASE_SHA should be honored, got ${ci_base}"
pass "CI BASE_SHA"

unset BASE_SHA
GITHUB_BASE_REF=main
github_base=$(resolve_changed_base "$repo")
[[ "$github_base" == "$main_tip" ]] ||
    fail "GITHUB_BASE_REF should resolve origin/main, got ${github_base}"
pass "GitHub base branch"

unset GITHUB_BASE_REF
print -r -- staged >"${repo}/staged.txt"
git -C "$repo" add staged.txt
print -r -- unstaged >>"${repo}/feature.txt"
print -r -- untracked >"${repo}/untracked.txt"
paths=$(collect_changed_files "$repo" "$base_sha" | sed '/^$/d' | sort -u)
for expected in feature.txt staged.txt untracked.txt; do
    print -r -- "$paths" | grep -Fx -- "$expected" >/dev/null ||
        fail "changed paths missing ${expected}"
done
pass "committed, staged, unstaged, and untracked paths"

map_output=$(zsh "${PROJECT_ROOT}/scripts/test.sh" module-map-check)
print -r -- "$map_output" | grep -F "测试模块映射合同通过" >/dev/null ||
    fail "registered test module mapping contract failed"
pass "registered test module mapping"

# Execute the runner's real selection functions with test execution replaced by
# recorders. These cases validate routing and failure propagation, not factoring.
python3 - "$PROJECT_ROOT" "$TEST_TMPDIR" <<'PY'
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(sys.argv[1])
runner = (root / 'scripts/test.sh').read_text()


def extract(pattern):
    match = re.search(pattern, runner, re.MULTILINE | re.DOTALL)
    if match is None:
        raise SystemExit(f'Cannot extract runner routing definition: {pattern}')
    return match.group(0)


definitions = '\n'.join(
    extract(rf'^typeset -A {name}\n.*?^\)')
    for name in ['MODULE_TESTS', 'MODULE_SLOW_TESTS', 'MODULE_CTEST_CONTRACTS',
                 'MODULE_DESC', 'MODULE_DEPS']
)
definitions += '\n' + extract(r'^typeset -a MODULE_ORDER\nMODULE_ORDER=\(.*?\)$')
definitions += '\n' + '\n'.join(
    extract(rf'^{name}\(\) \{{\n.*?^\}}')
    for name in ['path_to_module', 'module_build_targets', 'do_module_build',
                 'do_module', 'do_changed']
)
# Use the production failure-count-to-exit conversion as well as its selector.
exit_status = extract(r'^if \(\( FAILED_TESTS > 0 \)\); then\n.*?^exit 0$')
recorders = r'''
set -eo pipefail
PROJECT_ROOT=$PWD
FAILED_TESTS=0
SKIPPED_TESTS=0
TOTAL_TESTS=0
MODE=module
SKIP_TEST=""
typeset -a REPORT_ENTRIES=()
typeset -a SMOKE_TESTS=(test_integer)
log_header() { :; }
log_section() { :; }
log_info() { :; }
log_warn() { :; }
log_fail() { :; }
log_success() { :; }
log_skip() { :; }
resolve_changed_base() { print fixture; }
collect_changed_files() { print -r -- "$ROUTE_PATHS"; }
do_build_selected_tests() { :; }
run_ctest_contract_group() { :; }
do_50d_probe_contracts() { print 'SELECTED:50d_contract'; }
do_smoke() { print 'SELECTED:smoke'; }
run_single_test() {
    print -r -- "SELECTED:$1"
    (( TOTAL_TESTS += 1 ))
    if [[ "$1" == "$SKIP_TEST" ]]; then
        (( SKIPPED_TESTS += 1 ))
        return 0
    fi
    if [[ "$1" == "$FAIL_TEST" ]]; then
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    return 0
}
'''


def route(paths, options, expected_siqs, *, expected_gnfs=0, fail_test='', skip_test='', mode='changed'):
    script = (recorders + '\n' + definitions + '\n'
              'ROUTE_PATHS=$1; FAIL_TEST=$2; SKIP_TEST=$3; MODE=$4; shift 4\n'
              'do_changed "$@"\n'
              'print -r -- "COUNTS:$FAILED_TESTS:$SKIPPED_TESTS"\n' + exit_status + '\n')
    result = subprocess.run(
        ['zsh', '-s', '--', '\n'.join(paths), fail_test, skip_test, mode, *options],
        input=script, text=True, capture_output=True, cwd=root, check=False, timeout=10,
    )
    selected = [line.removeprefix('SELECTED:') for line in result.stdout.splitlines()
                if line.startswith('SELECTED:')]
    expected_status = int(bool(fail_test))
    if (result.returncode != expected_status
            or selected.count('test_siqs_e2e') != expected_siqs
            or selected.count('test_gnfs_e2e') != expected_gnfs
            or f'COUNTS:{int(bool(fail_test))}:{int(bool(skip_test))}' not in result.stdout.splitlines()):
        raise SystemExit(f'Routing failed for {paths!r} {options!r}: '
                         f'exit={result.returncode}, selected={selected!r}\n'
                         f'{result.stderr}')
    return selected


header = 'include/gnfs/siqs/siqs.hpp'
e2e_source = 'tests/test_siqs_e2e.cpp'
route([header], ['--deep'], 1)
route([e2e_source], ['--deep'], 1)
route(['src/siqs/shadow_proof_rss_campaign_journal_store_posix.cpp'], ['--deep'], 1)
route([header, e2e_source, 'tests/test_siqs.cpp', header], ['--deep'], 1)
route([header], [], 0)
route([e2e_source], [], 0)
indirect = route(['include/gnfs/util/joining_thread.hpp'], ['--deep'], 0, expected_gnfs=1)
if 'test_siqs' not in indirect:
    raise SystemExit('Utility fixture must reach SIQS through its module dependency')
for excluded in ['test_kleinjung_large', 'test_ecm_brent_suyama_bench',
                 'test_structured_ooc_scale']:
    if excluded in indirect:
        raise SystemExit(f'Deep selection unexpectedly enabled slow module test {excluded}')
route(['include/gnfs/core/integer.hpp'], ['--deep'], 0, expected_gnfs=1)
route([header, 'include/gnfs/core/integer.hpp'], ['--deep'], 1, expected_gnfs=1)
if route(['docs/algorithms/siqs-design.md'], ['--deep'], 0) != ['smoke']:
    raise SystemExit('Unmapped documentation must retain the smoke fallback')
if route([], ['--deep'], 0) != ['smoke']:
    raise SystemExit('No changes must retain the smoke fallback')
route([header], ['--deep'], 1, fail_test='test_siqs_e2e')
if 'test_int_polynomial' not in route(['tests/test_int_polynomial.cpp'], [], 0):
    raise SystemExit('Catalog routing must use the real module iteration order')
print('[PASS] changed selection, SIQS deep E2E, and failure propagation')

# Build failures must stop each changed-mode branch even when its caller has
# disabled errexit with || true. No stale binary or CTest may execute afterward.
for paths, unmapped in [([], False), (['docs/note.md'], False),
                        (['CMakeLists.txt'], True), ([header], False)]:
    driver = (recorders + '\n' + definitions + '\n'
              'ROUTE_PATHS=$1; FAIL_TEST=""\n'
              'do_build_selected_tests() { print BUILD_FAILED; return 41; }\n'
              + ('path_to_module() { :; }\n' if unmapped else '')
              + 'do_changed --deep || true\n')
    result = subprocess.run(['zsh', '-s', '--', '\n'.join(paths)], input=driver,
                            text=True, capture_output=True, cwd=root, timeout=10, check=True)
    if result.stdout.splitlines().count('BUILD_FAILED') != 1 or 'SELECTED:' in result.stdout:
        raise SystemExit(f'Failed selective build reached execution: {paths!r}\n'
                         f'{result.stdout}{result.stderr}')
print('[PASS] changed build failures stop execution inside an ignored-status caller')

# Module requests are the coverage oracle: changed selection may remove repeated
# no-argument calls, but may not remove any member of the selected module union.
def module_selection(modules):
    result = subprocess.run(
        ['zsh', '-s', '--', *modules], input=recorders + '\n' + definitions
        + '\nFAIL_TEST=""\ndo_module "$@"\n' + exit_status,
        text=True, capture_output=True, cwd=root, timeout=10, check=True)
    return [line.removeprefix('SELECTED:') for line in result.stdout.splitlines()
            if line.startswith('SELECTED:')]

api = module_selection(['api'])
for options in [[], ['--deep']]:
    selected = route(['tests/test_api.cpp'], options, 0)
    if set(selected) != set(api) or selected.count('test_api') != 1:
        raise SystemExit('Direct API changes lost coverage or repeated the API suite')

paths = ['include/gnfs/core/integer.hpp', 'tests/test_int_polynomial.cpp']
ordinary = module_selection(['core', 'polynomial']) + ['test_gnfs_e2e']
selected = route(paths, [], 0, expected_gnfs=1)
if set(selected) != set(ordinary) or len(selected) != len(set(selected)):
    raise SystemExit('Overlapping changed modules lost coverage or repeated a shared test')
if ordinary.count('test_regressions') != 2:
    raise SystemExit('Explicit module requests must preserve their existing repetition')

paths = ['include/gnfs/core/integer.hpp', 'tests/test_api.cpp']
ordinary = module_selection(['core', 'polynomial', 'factor_base', 'sieve', 'cofactor',
                             'relation', 'linalg', 'sqrt', 'api']) + ['test_gnfs_e2e']
for mode in ['changed', 'diff']:
    selected = route(paths, ['--deep'], 0, expected_gnfs=1, mode=mode)
    if set(selected) != set(ordinary) or len(selected) != len(set(selected)):
        raise SystemExit(f'{mode} deep changed the module union or repeated shared tests')
    for shared in ['test_api', 'test_edge_cases', 'test_regressions',
                   'test_integration', 'test_relation_reduction_engine']:
        if selected.count(shared) != 1:
            raise SystemExit(f'{mode} did not select shared {shared} exactly once')
route(paths, ['--deep'], 0, expected_gnfs=1, fail_test='test_integration')
route(paths, ['--deep'], 0, expected_gnfs=1, skip_test='test_edge_cases')
for path in ['include/gnfs/sieve/lattice_sieve.hpp', 'include/gnfs/relation/filter.hpp']:
    selected = route([path], ['--deep'], 0, expected_gnfs=1)
    if selected.count('test_api') != 1:
        raise SystemExit('Cascaded API coverage must remain selected exactly once')

# Each module must retain its own failure/skip outcome even when a shared binary
# has only one execution and global report entry. Invalid modules remain errors.
for mode, failure, skip, modules, expected in [
        ('changed', '', '', ['left', 'right'], 'RESULT:0:1:0:0'),
        ('changed', 'test_shared', '', ['left', 'right'], 'RESULT:2:1:1:0'),
        ('diff', '', 'test_shared', ['left', 'right'], 'RESULT:0:1:0:1'),
        ('changed', 'test_shared', '', ['left', 'missing', 'right', 'missing'], 'RESULT:4:3:3:0'),
        ('module', 'test_shared', '', ['left', 'right'], 'RESULT:2:2:2:0')]:
    driver = (recorders + '\n' + definitions + '\n'
              'MODE=$1; FAIL_TEST=$2; SKIP_TEST=$3; shift 3\n'
              'MODULE_TESTS=(left test_shared right test_shared)\n'
              'log_info() { if [[ "$mod" == left || "$mod" == right ]] && (( mod_total == 1 )); '
              'then print -r -- "SUMMARY:$mod:$mod_skip"; fi; }\n'
              'if do_module "$@"; then module_exit=0; else module_exit=$?; fi\n'
              'print -r -- "RESULT:$module_exit:$TOTAL_TESTS:$FAILED_TESTS:$SKIPPED_TESTS"\n')
    result = subprocess.run(['zsh', '-s', '--', mode, failure, skip, *modules], input=driver,
                            text=True, capture_output=True, cwd=root, timeout=10, check=True)
    if expected not in result.stdout.splitlines():
        raise SystemExit(f'Module result reuse changed accounting: {result.stdout}{result.stderr}')
    if skip and not {'SUMMARY:left:1', 'SUMMARY:right:1'}.issubset(result.stdout.splitlines()):
        raise SystemExit(f'Shared skip disappeared from a module summary: {result.stdout}')
print('[PASS] changed module union, shared outcomes, and explicit repetitions')

# Use the real binary-result/retry function with only its bounded process adapter
# replaced. Reusing a module result must not replace the explicit retry policy or
# let a fail-fast exit reach the result cache.
retry_build = Path(sys.argv[2]) / 'retry build'
retry_build.mkdir()
retry_binary = retry_build / 'test_shared'
retry_binary.write_text('#!/bin/sh\nexit 0\n')
retry_binary.chmod(0o755)
retry_driver = (recorders + '\n' + definitions + '\n'
                + extract(r'^run_single_test\(\) \{\n.*?^\}') + r"""
BUILD_DIR=$1; MODE=$2; ADAPTER_RESULT=$3; FAIL_FAST=$4; REPEAT_MODULES=$5
PASSED_TESTS=0
RETRY_COUNT=1
(( FAIL_FAST )) && RETRY_COUNT=0
TIMEOUT=3; TIMEOUT_EXPLICIT=0; QUIET=1; VERBOSE=0
DIM=""; RESET=""; RED=""
RUN_ATTEMPTS=0
MODULE_TESTS=(left test_shared right test_shared)
typeset -A TEST_TIMEOUT=(test_shared 3) TEST_TIER=(test_shared instant)
timer_start_ms() { print 0; }
format_duration() { print 0; }
run_with_timeout() {
    [[ $# == 2 && "$1" == 3 && "$2" == "$BUILD_DIR/test_shared" ]] || exit 21
    (( RUN_ATTEMPTS += 1 ))
    RUN_OUTPUT=""
    case "$ADAPTER_RESULT" in
        skip) return 77 ;;
        fail) return 1 ;;
        retry) (( RUN_ATTEMPTS >= 2 )) ;;
    esac
}
finish_fail_fast() {
    print -r -- "STOP:$RUN_ATTEMPTS:$TOTAL_TESTS:$FAILED_TESTS:${#REPORT_ENTRIES}"
    exit 17
}
for (( repeat_module = 0; repeat_module < REPEAT_MODULES; repeat_module++ )); do
    if do_module left right; then module_exit=0; else module_exit=$?; fi
done
print -r -- "RESULT:$RUN_ATTEMPTS:$module_exit:$TOTAL_TESTS:$PASSED_TESTS:$FAILED_TESTS:$SKIPPED_TESTS:${#REPORT_ENTRIES}"
""")
for mode, outcome, fail_fast, repeats, exit_code, expected in [
        ('changed', 'retry', '0', '1', 0, 'RESULT:2:0:1:1:0:0:1'),
        ('changed', 'retry', '0', '2', 0, 'RESULT:3:0:2:2:0:0:2'),
        ('module', 'retry', '0', '1', 0, 'RESULT:3:0:2:2:0:0:2'),
        ('diff', 'skip', '0', '1', 0, 'RESULT:1:0:1:0:0:1:1'),
        ('changed', 'fail', '1', '1', 17, 'STOP:1:1:1:1')]:
    result = subprocess.run(['zsh', '-s', '--', str(retry_build), mode, outcome, fail_fast, repeats],
                            input=retry_driver, text=True, capture_output=True, cwd=root, timeout=10)
    if result.returncode != exit_code or expected not in result.stdout.splitlines():
        raise SystemExit(f'Retry/skip/fail-fast behavior changed: {result.stdout}{result.stderr}')
print('[PASS] explicit retries, native exit-77 accounting, and fail-fast remain intact')

# Exercise the full parser and dispatcher. A fake CMake terminates accepted
# selections before any configuration, build, or mathematical test can run.
fixture = Path(sys.argv[2])
bin_dir = fixture / 'bin'
bin_dir.mkdir()
cmake = bin_dir / 'cmake'
cmake.write_text('#!/bin/sh\nprintf "called\\n" >> "$GNFS_TEST_CMAKE_MARKER"\nexit 99\n')
cmake.chmod(0o755)
marker = fixture / 'cmake-called'
environment = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                   GNFS_BUILD_DIR=str(fixture / 'unused-build'),
                   GNFS_TEST_CMAKE_MARKER=str(marker), GNFS_PYTHON_EXECUTABLE=sys.executable)
invalid = [
    ['changed', '--slow'], ['changed', '--deep', '--slow'],
    ['changed', '--slow', '--deep'], ['diff', '--slow'],
    ['changed', '--deep', '--deep'], ['changed', 'siqs'],
    ['changed', '--depe'], ['--no-build', 'changed', '--slow'],
    ['changed', '--save'], ['changed', '--compare'],
    ['changed', '--deep', '--save'], ['changed', '--compare', '--deep'],
    ['diff', '--save'], ['diff', '--deep', '--compare'],
    ['--save', 'changed'], ['--compare', 'diff'],
    ['--no-build', 'changed', '--save'], ['--no-build', 'diff', '--compare'],
]
valid = [
    ['changed'], ['changed', '--deep'], ['--deep', 'changed'],
    ['diff', '--deep'], ['module', 'siqs', '--slow'],
    ['bench', '--save'], ['bench', '--compare'],
]


def check_argument_case(options):
    if marker.exists():
        marker.unlink()
    result = subprocess.run(
        ['zsh', str(root / 'scripts/test.sh'), '--no-color', *options],
        text=True, capture_output=True, cwd=fixture, env=environment, timeout=10,
    )
    rejected = options in invalid
    # changed catches build failures at its selection boundary; direct build
    # dispatchers retain CMake's original status.
    expected_status = 2 if rejected else (1 if {'changed', 'diff'} & set(options) else 99)
    calls = marker.read_text().splitlines() if marker.exists() else []
    if (result.returncode != expected_status
            or calls != ([] if rejected else ['called'])
            or (rejected and '参数错误' not in result.stdout + result.stderr)):
        raise SystemExit(f'Argument validation failed for {options!r}: '
                         f'exit={result.returncode}, cmake_calls={calls!r}\n'
                         f'{result.stdout}{result.stderr}')


for options in invalid + valid:
    check_argument_case(options)

# A Debug request on a cached Release build takes the reconfiguration path.
# It must fail at the same boundary before attempting to build.
cached_build = Path(environment['GNFS_BUILD_DIR'])
cached_build.mkdir(exist_ok=True)
(cached_build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\n')
check_argument_case(['changed'])

module = subprocess.run(
    ['zsh', '-s'], input=recorders + '\n' + definitions
    + '\nFAIL_TEST=""\ndo_module siqs --slow\n' + exit_status,
    text=True, capture_output=True, cwd=root, timeout=10, check=True,
)
if module.stdout.splitlines().count('SELECTED:test_siqs_e2e') != 1:
    raise SystemExit('Explicit module --slow must still select SIQS E2E once')
print('[PASS] changed arguments fail before CMake; module --slow remains supported')
PY

# Run the complete runner against an isolated repository and fake build tools.
# Binaries exist only after their target is built, so execution and JSON reports
# independently prove that the selected build covered the requested behavior.
python3 - "$PROJECT_ROOT" "${TEST_TMPDIR}/selective runner" <<'PY'
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

project, fixture = map(Path, sys.argv[1:])
(fixture / 'scripts/lib').mkdir(parents=True)
(fixture / 'scripts/test-catalog').mkdir()
for name in ['lib/changed_files.zsh', 'lib/process_tree_timeout.zsh',
             'test-catalog/windows-unavailable.txt']:
    shutil.copy2(project / 'scripts' / name, fixture / 'scripts' / name)
runner = (project / 'scripts/test.sh').read_text()
optional = (project / 'scripts/test-catalog/windows-unavailable.txt').read_text().split()[0]
modules = {
    'core': ['test_integer', 'test_regressions'],
    'util': ['test_small_vector'],
    'polynomial': ['test_int_polynomial', 'test_regressions'],
    'sieve': ['test_sieve_basic', optional],
    'relation': ['test_relation_collector'],
    'api': ['test_api'],
    'siqs': ['test_siqs'],
}
slow = {'polynomial': ['test_kleinjung_large'], 'siqs': ['test_siqs_e2e']}
smoke = ['test_integer', 'test_small_vector']


def require(condition, detail):
    if not condition:
        raise SystemExit(detail)


def replace(pattern, replacement):
    global runner
    runner, count = re.subn(pattern, lambda _: replacement, runner,
                            count=1, flags=re.MULTILINE | re.DOTALL)
    require(count == 1, f'Cannot prepare runner fixture: {pattern}')


for name, table in [('MODULE_TESTS', modules), ('MODULE_SLOW_TESTS', slow),
                    ('MODULE_DEPS', {'core': ['polynomial', 'api'], 'util': ['siqs']})]:
    replace(rf'^{name}=\(\n.*?^\)', name + '=(\n' + '\n'.join(
        f'    {key} "{" ".join(values)}"' for key, values in table.items()) + '\n)')
replace(r'^MODULE_ORDER=\(.*?\)$', 'MODULE_ORDER=(' + ' '.join(modules) + ')')
# The 50-digit schema contract is covered elsewhere. At this execution boundary,
# require the selected probe binary to exist, just like a real contract needs it.
replace(r'^self_check_50d_probe_contracts\(\) \{\n.*?^\}',
        'self_check_50d_probe_contracts() {\n'
        '    "${BUILD_DIR}/test_structured_ooc_50d_probe" fixture-contract\n}')
(fixture / 'scripts/test.sh').write_text(runner)
(fixture / 'scripts/test-catalog/smoke.txt').write_text('\n'.join(smoke) + '\n')
sources = ['include/gnfs/core/integer.hpp', 'include/gnfs/util/safe_math.hpp',
           'include/gnfs/siqs/siqs.hpp', 'CMakeLists.txt', 'docs/note.md']
for name in sources:
    source = fixture / name
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text('// baseline\n')
(fixture / '.gitignore').write_text('.cache/\n')
for args in [['init', '-q'], ['add', '.'],
             ['-c', 'user.name=Runner Fixture', '-c', 'user.email=fixture@example.invalid',
              '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture baseline']]:
    subprocess.run(['git', *args], cwd=fixture, check=True, stdout=subprocess.DEVNULL)
base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=fixture, text=True).strip()
cache = fixture / '.cache'
bin_dir = cache / 'bin'
bin_dir.mkdir(parents=True)
common = "import json, os, pathlib, subprocess, sys\n"
common += "with open(os.environ['GNFS_STUB_LOG'], 'a') as stream:\n"
common += "    stream.write(json.dumps([pathlib.Path(sys.argv[0]).name, *sys.argv[1:]]) + '\\n')\n"
child = ('#!' + sys.executable + '\n' + common
         + "if pathlib.Path(sys.argv[0]).name == 'gnfs_test_process_supervisor':\n"
         + "    raise SystemExit(subprocess.call(sys.argv[sys.argv.index('--') + 1:]))\n")


def executable(path, body):
    path.write_text('#!' + sys.executable + '\n' + common + body)
    path.chmod(0o755)


executable(bin_dir / 'cmake', r'''
args = sys.argv[1:]
if '--build' not in args:
    build = pathlib.Path(args[args.index('-B') + 1])
    build.mkdir(parents=True, exist_ok=True)
    build_type = next(a.split('=', 1)[1] for a in args if a.startswith('-DCMAKE_BUILD_TYPE='))
    (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=' + build_type + '\n')
    platform = os.environ['GNFS_STUB_WIN32']
    if platform != 'missing':
        (build / 'gnfs-test-platform.txt').write_text('WIN32=' + platform + '\n')
    raise SystemExit(int(os.environ['GNFS_STUB_CONFIGURE_EXIT']))
build = pathlib.Path(args[args.index('--build') + 1])
if int(os.environ['GNFS_STUB_BUILD_EXIT']):
    raise SystemExit(int(os.environ['GNFS_STUB_BUILD_EXIT']))
targets = args[args.index('--target') + 1:] if '--target' in args else json.loads(os.environ['GNFS_STUB_ALL_TARGETS'])
if os.environ['GNFS_STUB_WIN32'] == '1' and os.environ['GNFS_STUB_OPTIONAL'] in targets:
    raise SystemExit('attempted to build a target CMake excludes on WIN32')
for name in targets:
    binary = build / name
    binary.write_text(os.environ['GNFS_STUB_CHILD'])
    binary.chmod(0o755)
    if name == 'gnfs_test_process_supervisor':
        dependency = build / 'bounded_child_process_fake_child'
        dependency.write_text(os.environ['GNFS_STUB_CHILD'])
        dependency.chmod(0o755)
''')
executable(bin_dir / 'ctest', r'''
args = sys.argv[1:]
build = pathlib.Path(args[args.index('--test-dir') + 1])
regex = args[args.index('-R') + 1]
required = []
if 'CLIReplConfig' in regex:
    required.append('gnfs')
if 'ProcessSupervisor' in regex:
    required.extend(['gnfs_test_process_supervisor', 'bounded_child_process_fake_child'])
for name in required:
    if not (build / name).is_file():
        raise SystemExit('CTest dependency not built: ' + name)
raise SystemExit(int(os.environ['GNFS_STUB_CTEST_EXIT']))
''')
executable(bin_dir / 'fswatch', "print('1')\n")
# Timing is irrelevant here and should not dominate subprocess fixture costs.
clock = bin_dir / 'clock'
clock.write_text('#!/bin/sh\nprintf "0\\n"\n')
clock.chmod(0o755)
host = bin_dir / 'uname'
host.write_text('#!/bin/sh\nprintf "%s\\n" "$GNFS_STUB_HOST"\n')
host.chmod(0o755)
all_targets = set(sum(modules.values(), []) + sum(slow.values(), []) + smoke)
all_targets.update(['test_gnfs_e2e', 'test_structured_ooc_50d_probe',
                    'gnfs', 'gnfs_test_process_supervisor', 'bounded_child_process_fake_child'])
case_index = 0


def invoke(options, *, changes=(), win32='0', host='Linux', build_exit=0,
           configure_exit=0, ctest_exit=0, prebuilt=False, cached_win32=None):
    global case_index
    case_index += 1
    for name in sources:
        (fixture / name).write_text('// baseline\n' + ('// modified\n' if name in changes else ''))
    build = cache / str(case_index) / 'build with spaces'
    build.mkdir(parents=True)
    if prebuilt or cached_win32 is not None:
        (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\n')
        initial_platform = win32 if cached_win32 is None else cached_win32
        (build / 'gnfs-test-platform.txt').write_text('WIN32=' + initial_platform + '\n')
    if prebuilt:
        for name in all_targets:
            binary = build / name
            binary.write_text(child)
            binary.chmod(0o755)
    log = build.parent / 'calls.jsonl'
    log.write_text('')
    env = {**os.environ, 'PATH': str(bin_dir) + os.pathsep + os.environ['PATH'],
           'GNFS_BUILD_DIR': str(build), 'GNFS_TEST_BASE_SHA': base,
           'GNFS_PYTHON_EXECUTABLE': str(clock), 'GNFS_STUB_LOG': str(log),
           'GNFS_STUB_WIN32': win32, 'GNFS_STUB_HOST': host,
           'GNFS_STUB_BUILD_EXIT': str(build_exit), 'GNFS_STUB_CONFIGURE_EXIT': str(configure_exit),
           'GNFS_STUB_CTEST_EXIT': str(ctest_exit),
           'GNFS_STUB_ALL_TARGETS': json.dumps(sorted(all_targets)),
           'GNFS_STUB_CHILD': child, 'GNFS_STUB_OPTIONAL': optional}
    env.pop('GNFS_TEST_PROCESS_SUPERVISOR', None)
    result = subprocess.run(['zsh', str(fixture / 'scripts/test.sh'), '--no-color', *options],
                            cwd=fixture, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=20)
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    report_path = build / 'test_report.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else None
    return result, calls, report


def successful(options, executed, *, cli=False, contracts=0, iterations=1, skipped=(), **kwargs):
    result, calls, report = invoke(options, **kwargs)
    require(result.returncode == 0, f'{options!r} failed:\n{result.stdout}')
    builds = [call for call in calls if call[:2] == ['cmake', '--build']]
    if '--no-build' in options:
        require(not any(call[0] == 'cmake' for call in calls), f'--no-build invoked CMake: {calls!r}')
    else:
        require(len(builds) == iterations, f'{options!r} built {len(builds)} times: {calls!r}')
        expected = set(executed) | {'gnfs_test_process_supervisor'}
        if cli:
            expected.add('gnfs')
        for call in builds:
            require('--target' in call, f'{options!r} performed a full build: {call!r}')
            targets = call[call.index('--target') + 1:]
            require(set(targets) == expected and len(targets) == len(expected),
                    f'{options!r} targets differ from selected union: {targets!r} != {sorted(expected)!r}')
    binaries = [call[0] for call in calls if call[0].startswith('test_')]
    require(Counter(binaries) == Counter(executed * iterations),
            f'{options!r} executed wrong tests: {binaries!r}')
    require(len([call for call in calls if call[0] == 'ctest']) == contracts * iterations,
            f'{options!r} lost or repeated CTest contracts: {calls!r}')
    if 'watch' in options:
        # Watch intentionally shows each iteration's summary without publishing
        # a final JSON report for an open-ended session.
        require(report is None, f'Watch unexpectedly published a final report: {report!r}')
        return calls
    require(report is not None and report['failed'] == 0 and
            report['passed'] == len(executed) + contracts and report['skipped'] == len(skipped),
            f'{options!r} report does not reflect execution: {report!r}')
    require({entry['name'] for entry in report['tests'] if entry['status'] == 'skip'} == set(skipped),
            f'{options!r} reported incorrect platform skips: {report!r}')
    return calls


successful(['module', 'core'], modules['core'])
successful(['mod', 'polynomial', '--slow'], modules['polynomial'] + slow['polynomial'])
successful(['module', 'core', 'polynomial', 'core'],
           modules['core'] + modules['polynomial'] + modules['core'])
successful(['module', 'all'], sum(modules.values(), []), cli=True, contracts=2)
successful(['module', 'api'], modules['api'], cli=True, contracts=1)
successful(['module', 'util'], modules['util'], contracts=1)
print('[PASS] module builds preserve default, slow, repeated, all, and CTest selections')

core = ['include/gnfs/core/integer.hpp']
siqs = ['include/gnfs/siqs/siqs.hpp']
successful(['changed'], modules['core'] + ['test_gnfs_e2e'], changes=core)
deep_core = list(dict.fromkeys(modules['core'] + modules['polynomial'] + modules['api']))
successful(['changed', '--deep'], deep_core + ['test_gnfs_e2e'], changes=core, cli=True, contracts=1)
successful(['diff', '--deep'], modules['siqs'] + ['test_siqs_e2e'], changes=siqs)
successful(['changed', '--deep'], modules['util'] + modules['siqs'],
           changes=['include/gnfs/util/safe_math.hpp'], contracts=1)
for changes in [[], ['docs/note.md']]:
    successful(['changed'], smoke, changes=changes, contracts=1)
successful(['changed'], modules['util'] + ['test_structured_ooc_50d_probe'],
           changes=['CMakeLists.txt'], contracts=1)
print('[PASS] changed builds cover cascades, smoke fallbacks, E2E, and 50-digit contracts')

# These cases start without a CMake cache or platform file. The build must use
# target metadata produced by configuration, even when uname suggests otherwise.
for platform, host_name in [('1', 'Linux'), ('0', 'MSYS_NT-10.0'),
                            ('missing', 'MINGW64_NT-10.0'), ('invalid', 'MSYS_NT-10.0')]:
    omitted = [optional] if platform == '1' else []
    selected = [name for name in modules['sieve'] if name not in omitted]
    successful(['module', 'sieve'], selected, win32=platform, host=host_name, skipped=omitted)
for previous, current in [('1', '0'), ('0', '1')]:
    omitted = [optional] if current == '1' else []
    selected = [name for name in modules['sieve'] if name not in omitted]
    successful(['module', 'sieve'], selected, win32=current, cached_win32=previous, skipped=omitted)
print('[PASS] target filtering uses configured WIN32 metadata rather than host uname')

for options, changes in [(['module', 'core'], []), (['changed'], core),
                          (['changed'], []), (['changed'], ['CMakeLists.txt'])]:
    for configure_exit, build_exit in [(43, 0), (0, 41)]:
        result, calls, report = invoke(options, changes=changes, configure_exit=configure_exit,
                                       build_exit=build_exit, prebuilt=bool(build_exit))
        require(result.returncode != 0 and not any(call[0] != 'cmake' for call in calls),
                f'Failed build reached stale binaries or CTest: {options!r}, {calls!r}\n{result.stdout}')
        require(report is None or (report['failed'] > 0 and report['passed'] == 0),
                f'Failed build published a successful report: {report!r}')
print('[PASS] configuration and build failures stop before stale tests or success reports')

successful(['--no-build', 'module', 'core'], modules['core'], prebuilt=True)
successful(['--no-build', 'changed', '--deep'], modules['siqs'] + ['test_siqs_e2e'],
           changes=siqs, prebuilt=True)
# A finite fswatch stream produces one initial run and exactly one changed run.
successful(['watch', 'module', 'core'], modules['core'], iterations=2)
successful(['watch', 'changed'], modules['core'] + ['test_gnfs_e2e'],
           changes=core, iterations=2)
result, calls, _ = invoke(['watch', 'unit'], ctest_exit=47)
require(result.returncode == 47 and len([call for call in calls if call[0] == 'ctest']) == 1
        and not any(call[0] == 'fswatch' for call in calls),
        f'Failed initial watch unit run continued or lost CTest exit status: {calls!r}\n{result.stdout}')
print('[PASS] --no-build avoids CMake and watch builds once per iteration')
PY

print -r -- "Changed-mode base contract passed."
