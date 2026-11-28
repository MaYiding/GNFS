#!/bin/sh
# Check the formatting of the C/C++ files a branch changed, with the clang-format
# that CI's Format job uses, so a format difference never costs a CI round.
#
# Usage: format_changed.sh [--fix] [<base>] [<clang-format>]
#   --fix          reformat the files in place instead of only checking
#   base           the ref the branch diverged from (default origin/main)
#   clang-format   the formatter to run (default $CLANG_FORMAT, then clang-format)
#
# CI installs clang-format 18. The Format job checks every file a pull request
# changes, so a file that drifted earlier fails as soon as a change touches it.
set -u

fix=false
if [ "${1:-}" = "--fix" ]; then
    fix=true
    shift
fi
base="${1:-origin/main}"
formatter="${2:-${CLANG_FORMAT:-clang-format}}"

command -v "$formatter" > /dev/null 2>&1 || { echo "clang-format not found: $formatter" >&2; exit 2; }
git rev-parse --verify --quiet "$base" > /dev/null || { echo "unknown ref: $base" >&2; exit 2; }

list=$(mktemp) || exit 2
trap 'rm -f "$list"' EXIT
# Only added, copied, modified, and renamed files; a deleted file has nothing to format.
git diff --name-only -z --diff-filter=ACMR "$base"...HEAD -- \
    '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' > "$list"
if [ ! -s "$list" ]; then
    echo "no C/C++ files changed since $base"
    exit 0
fi

# xargs would run clang-format without arguments on empty input, and clang-format
# then reads standard input; the size check above keeps the list non-empty.
mode="--dry-run"
if $fix; then
    mode="-i"
fi
# A leading ./ keeps clang-format from reading a name that begins with @ as an option.
count=$(tr -cd '\0' < "$list" | wc -c | tr -d ' ')
tr '\0' '\n' < "$list" | sed 's|^|./|' | tr '\n' '\0' | xargs -0 "$formatter" $mode --Werror -- > /dev/null 2>&1
status=$?
if $fix; then
    echo "clang-format: reformatted $count changed file(s) ($formatter)"
elif [ "$status" -eq 0 ]; then
    echo "clang-format: $count changed file(s) are formatted ($formatter)"
else
    echo "clang-format: formatting differences in the changed files ($formatter):"
    tr '\0' '\n' < "$list" | sed 's|^|./|' | tr '\n' '\0' | xargs -0 "$formatter" --dry-run --Werror -- 2>&1 |
        grep -E '^[^ ]*\.(c|cc|cpp|cxx|h|hh|hpp|hxx):[0-9]+:[0-9]+: (error|warning):' | sort -u | head -40
    exit 1
fi
