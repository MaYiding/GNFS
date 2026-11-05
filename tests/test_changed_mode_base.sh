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
log_header() { :; }
log_section() { :; }
log_info() { :; }
log_warn() { :; }
log_fail() { :; }
log_success() { :; }
resolve_changed_base() { print fixture; }
collect_changed_files() { print -r -- "$ROUTE_PATHS"; }
run_ctest_contract_group() { :; }
do_50d_probe_contracts() { print 'SELECTED:50d_contract'; }
do_smoke() { print 'SELECTED:smoke'; }
run_single_test() {
    print -r -- "SELECTED:$1"
    (( TOTAL_TESTS += 1 ))
    if [[ "$1" == "$FAIL_TEST" ]]; then
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    return 0
}
'''


def route(paths, options, expected_siqs, *, expected_gnfs=0, fail_test=''):
    script = (recorders + '\n' + definitions + '\n'
              'ROUTE_PATHS=$1; FAIL_TEST=$2; shift 2\n'
              'do_changed "$@"\n' + exit_status + '\n')
    result = subprocess.run(
        ['zsh', '-s', '--', '\n'.join(paths), fail_test, *options],
        input=script, text=True, capture_output=True, cwd=root, check=False, timeout=10,
    )
    selected = [line.removeprefix('SELECTED:') for line in result.stdout.splitlines()
                if line.startswith('SELECTED:')]
    expected_status = int(bool(fail_test))
    if (result.returncode != expected_status
            or selected.count('test_siqs_e2e') != expected_siqs
            or selected.count('test_gnfs_e2e') != expected_gnfs):
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
for options in invalid + valid:
    if marker.exists():
        marker.unlink()
    result = subprocess.run(
        ['zsh', str(root / 'scripts/test.sh'), '--no-color', *options],
        text=True, capture_output=True, cwd=fixture, env=environment, timeout=10,
    )
    rejected = options in invalid
    if (result.returncode != (2 if rejected else 99)
            or marker.exists() == rejected
            or (rejected and '参数错误' not in result.stdout + result.stderr)):
        raise SystemExit(f'Argument validation failed for {options!r}: '
                         f'exit={result.returncode}, cmake_called={marker.exists()}\n'
                         f'{result.stdout}{result.stderr}')

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
