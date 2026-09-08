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

print -r -- "Changed-mode base contract passed."
