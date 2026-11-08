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
import shutil
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
    for name in ['MODULE_SLOW_TESTS', 'MODULE_CTEST_CONTRACTS', 'MODULE_DESC']
)
definitions += '\n' + extract(r'^typeset -A MODULE_TESTS MODULE_DEPS\n.*?^done < [^\n]+')
definitions += '\n' + extract(r'^typeset -a MODULE_ORDER\nMODULE_ORDER=\(.*?\)$')
definitions += '\n' + '\n'.join(
    extract(rf'^{name}\(\) \{{\n.*?^\}}')
    for name in ['path_to_module', 'do_module', 'do_changed']
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
log_header() { :; }
log_section() { :; }
log_info() { :; }
log_warn() { :; }
log_fail() { :; }
log_success() { :; }
log_skip() { :; }
resolve_changed_base() { print fixture; }
collect_changed_files() { print -r -- "$ROUTE_PATHS"; }
run_ctest_contract_group() { print -r -- "CONTRACT:$1"; }
do_build() { print -r -- "BUILD:$*"; }
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


module_catalog = {}
for line in (root / 'scripts/test-catalog/modules.txt').read_text().splitlines():
    module, tests, downstream = line.split('|')
    module_catalog[module] = (set(tests.split()), downstream.split())


def build_closure(targets):
    expanded = set()
    covered_modules = set()
    for target in targets:
        if target.startswith(('gnfs_local_module_', 'gnfs_local_deep_')):
            module = target.split('_', 3)[-1]
            modules = [module]
            if target.startswith('gnfs_local_deep_'):
                modules.extend(module_catalog[module][1])
            if covered_modules.intersection(modules):
                raise SystemExit(f'Make would traverse an overlapping module group: {targets!r}')
            covered_modules.update(modules)
            for mod in modules:
                expanded.update(module_catalog[mod][0])
                expanded.add('gnfs_test_process_supervisor')
                if mod == 'api':
                    expanded.add('gnfs')
        else:
            expanded.add(target)
    return expanded


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
    builds = [line.removeprefix('BUILD:').split() for line in result.stdout.splitlines()
              if line.startswith('BUILD:')]
    if selected == ['smoke']:
        expected_targets = {'gnfs_local_gate_quick'}
    else:
        expected_targets = set(selected) | {'gnfs_test_process_supervisor'}
        if '50d_contract' in expected_targets:
            expected_targets.remove('50d_contract')
            expected_targets.add('test_structured_ooc_50d_probe')
        if 'test_cli_event_stream' in selected:
            expected_targets.add('gnfs')
    if len(builds) != 1 or len(builds[0]) != len(set(builds[0])) or build_closure(builds[0]) != expected_targets:
        raise SystemExit(f'Build targets differ from execution for {paths!r} {options!r}: '
                         f'{builds!r} versus {expected_targets!r}')
    return selected


header = 'include/gnfs/siqs/siqs.hpp'
e2e_source = 'tests/test_siqs_e2e.cpp'
route([header], ['--deep'], 1)
route([e2e_source], ['--deep'], 1)
route(['CMakeLists.txt'], [], 0)
route(['tests/test_structured_ooc_50d_probe.cpp'], [], 0, expected_gnfs=1)
route(['src/siqs/shadow_proof_rss_campaign_journal_store_posix.cpp'], ['--deep'], 1)
route([header, e2e_source, 'tests/test_siqs.cpp', header], ['--deep'], 1)
route([header], [], 0)
route([e2e_source], [], 0)
indirect = route(['include/gnfs/util/joining_thread.hpp'], ['--deep'], 0, expected_gnfs=1)
route(['include/gnfs/util/joining_thread.hpp', 'include/gnfs/core/integer.hpp',
       'include/gnfs/sieve/lattice_sieve.hpp', 'include/gnfs/linalg/matrix.hpp'],
      ['--deep'], 0, expected_gnfs=1)
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

# Exercise the CMake closure itself, including the target-platform exclusions,
# using small stamp-producing targets without compiling any language.
if cmake := shutil.which('cmake'):
    source = Path(sys.argv[2]) / 'module groups'
    source.mkdir()
    catalog = source / 'modules.txt'
    catalog.write_text('core|test_integer test_shared test_optional|api\n'
                       'api|test_cli test_shared|other\nother|test_other|\n')
    cmake_source = (root / 'CMakeLists.txt').read_text()
    definition = re.search(r'(?ms)^\s*function\(gnfs_add_local_module_targets\b.*?^\s*endfunction\(\)',
                           cmake_source)
    if definition is None:
        raise SystemExit('CMake module target definition missing')
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(ModuleTargets NONE)\n'
        'set(WIN32 "${SIMULATE_WINDOWS}")\n'
        'set(outputs gnfs_test_process_supervisor gnfs test_integer test_shared test_cli test_other)\n'
        'if(NOT WIN32)\n  list(APPEND outputs test_optional)\nendif()\n'
        'foreach(name IN LISTS outputs)\n'
        '  add_custom_target(${name} COMMAND "${CMAKE_COMMAND}" -E touch '
        '"${CMAKE_BINARY_DIR}/${name}.stamp")\nendforeach()\n'
        + definition.group(0) + '\n'
        + 'gnfs_add_local_module_targets("${CMAKE_CURRENT_SOURCE_DIR}/modules.txt" test_optional)\n')
    for windows in ('OFF', 'ON'):
        build = source / windows
        subprocess.run([cmake, '-S', str(source), '-B', str(build),
                        '-DSIMULATE_WINDOWS=' + windows], check=True, capture_output=True)
        for target in ('gnfs_local_module_core', 'gnfs_local_deep_core'):
            subprocess.run([cmake, '--build', str(build), '--target', target],
                           check=True, capture_output=True)
            stamps = list(build.glob('*.stamp'))
            expected = {'gnfs_test_process_supervisor', 'test_integer', 'test_shared'}
            if windows == 'OFF':
                expected.add('test_optional')
            if target == 'gnfs_local_deep_core':
                expected.update({'gnfs', 'test_cli'})
            if {p.stem for p in stamps} != expected:
                raise SystemExit(f'Incorrect CMake closure for {target}, Windows={windows}: {stamps!r}')
            for stamp in stamps:
                stamp.unlink()
    catalog.write_text(catalog.read_text().replace('test_integer', 'test_missing'))
    failed = subprocess.run([cmake, '-S', str(source), '-B', str(source / 'missing'),
                             '-DSIMULATE_WINDOWS=ON'], text=True, capture_output=True)
    if failed.returncode == 0 or 'test_missing' not in failed.stderr:
        raise SystemExit('A missing required module test must fail CMake generation')
    print('[PASS] actual module targets share helpers and honor one-level cascades and Windows exclusions')


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
    calls = marker.read_text().splitlines() if marker.exists() else []
    if (result.returncode != (2 if rejected else 99)
            or calls != ([] if rejected else ['called'])
            or (rejected and '参数错误' not in result.stdout + result.stderr)):
        raise SystemExit(f'Argument validation failed for {options!r}: '
                         f'exit={result.returncode}, cmake_calls={calls!r}\n'
                         f'{result.stdout}{result.stderr}')


for options in invalid + valid:
    check_argument_case(options)

# A Debug request on a cached Release build takes the reconfiguration path.
# It must propagate the same CMake failure before attempting to build.
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

print -r -- "Changed-mode base contract passed."
