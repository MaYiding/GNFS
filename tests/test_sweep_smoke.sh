#!/usr/bin/env zsh
# ╔══════════════════════════════════════════════════════════════════╗
# ║  Shell-level smoke test for sweep_* scripts                        ║
# ║  Verifies dry-run paths work and markdown reports are well-formed  ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# Pure shell test, no GNFS execution. Runs each sweep entry point with
# `--dry-run --no-build` and asserts:
#
#   1. exit code 0
#   2. produced markdown file exists
#   3. markdown contains the per-N "## Results:" header
#   4. markdown contains the "## Best ENV Value per N Size" recap
#   5. analyzer reads the produced markdown without error
#
# Designed to be cheap (under 5 s) so it can run in any regular CI lane.

set -eo pipefail

PROJECT_ROOT="${0:A:h:h}"
RESULTS_DIR="${PROJECT_ROOT}/bench/results"

PASS=0
FAIL=0
FAILED_TESTS=()

# ── Helpers ──
run_check() {
    local name="$1"; shift
    if "$@"; then
        echo "  [PASS] $name"
        PASS=$(( PASS + 1 ))
    else
        echo "  [FAIL] $name"
        FAIL=$(( FAIL + 1 ))
        FAILED_TESTS+=("$name")
    fi
}

assert_file_exists() {
    [[ -f "$1" ]]
}

assert_file_contains() {
    grep -q "$2" "$1"
}

assert_exit_zero() {
    "$@" > /dev/null 2>&1
}

# ── Setup ──
mkdir -p "$RESULTS_DIR"
TMPDIR_BASE="${TMPDIR:-/tmp}/sweep_smoke_$$"
mkdir -p "$TMPDIR_BASE"

cleanup() {
    rm -rf "$TMPDIR_BASE"
    # Remove only the test-mode reports.
    rm -f "${RESULTS_DIR}"/sweep_test_smoke_*.md
    rm -f "${RESULTS_DIR}"/combo_test_smoke_*.md
}
trap cleanup EXIT

echo "=== sweep_full.sh dry-run smoke ==="
SWEEP_OUT="${RESULTS_DIR}/sweep_test_smoke_$(date +%s).md"
run_check "sweep_full.sh --bit 40 --dry-run --no-build (exit 0)" \
    assert_exit_zero "${PROJECT_ROOT}/scripts/sweep_full.sh" \
    --bit 40 --dry-run --no-build --output "$SWEEP_OUT"

run_check "report file produced" assert_file_exists "$SWEEP_OUT"
run_check "report has 'Results: 40-bit' header" \
    assert_file_contains "$SWEEP_OUT" "## Results: 40-bit"
run_check "report has 'Best ENV Value per N Size' section" \
    assert_file_contains "$SWEEP_OUT" "## Best ENV Value per N Size"
run_check "report has metadata block" \
    assert_file_contains "$SWEEP_OUT" "| Host |"
run_check "report has baseline row" \
    assert_file_contains "$SWEEP_OUT" "| baseline | (none) |"

echo
echo "=== sweep_full.sh --env-set filter ==="
SWEEP_OUT2="${RESULTS_DIR}/sweep_test_smoke_filter_$(date +%s).md"
run_check "filter to single ENV (exit 0)" \
    assert_exit_zero "${PROJECT_ROOT}/scripts/sweep_full.sh" \
    --bit 40 --dry-run --no-build \
    --env-set "GNFS_POLY_BAI_BRENT" \
    --output "$SWEEP_OUT2"

run_check "filtered report contains POLY_BAI_BRENT" \
    assert_file_contains "$SWEEP_OUT2" "GNFS_POLY_BAI_BRENT"
# Filtered report must NOT contain ENVs that were filtered out.
if grep -q "GNFS_OOC_RELATIONS" "$SWEEP_OUT2"; then
    echo "  [FAIL] filter exclusion (GNFS_OOC_RELATIONS leaked)"
    FAIL=$(( FAIL + 1 ))
    FAILED_TESTS+=("filter exclusion")
else
    echo "  [PASS] filter exclusion (GNFS_OOC_RELATIONS absent)"
    PASS=$(( PASS + 1 ))
fi

echo
echo "=== sweep_combo.sh dry-run smoke ==="
COMBO_OUT="${RESULTS_DIR}/combo_test_smoke_$(date +%s).md"
run_check "sweep_combo.sh dry-run (exit 0)" \
    assert_exit_zero "${PROJECT_ROOT}/scripts/sweep_combo.sh" \
    --bit 40 --dry-run --no-build \
    --env-a GNFS_POLY_BAI_BRENT \
    --env-b GNFS_ECM_BRENT_SUYAMA \
    --output "$COMBO_OUT"

run_check "combo report produced" assert_file_exists "$COMBO_OUT"
run_check "combo report contains 2x2 header" \
    assert_file_contains "$COMBO_OUT" "## Combo:"
run_check "combo report contains both ENVs" \
    assert_file_contains "$COMBO_OUT" "GNFS_POLY_BAI_BRENT"
run_check "combo report contains ENV-B name" \
    assert_file_contains "$COMBO_OUT" "GNFS_ECM_BRENT_SUYAMA"

echo
echo "=== sweep_combo.sh missing --env-a error ==="
if "${PROJECT_ROOT}/scripts/sweep_combo.sh" --bit 40 --dry-run --no-build \
    --env-b GNFS_ECM_BRENT_SUYAMA > /dev/null 2>&1; then
    echo "  [FAIL] missing --env-a should error"
    FAIL=$(( FAIL + 1 ))
    FAILED_TESTS+=("missing --env-a")
else
    echo "  [PASS] missing --env-a errors out"
    PASS=$(( PASS + 1 ))
fi

echo
echo "=== sweep_full.sh and sweep_combo.sh --help ==="
HELP_FULL_OUT="$TMPDIR_BASE/help_full.out"
"${PROJECT_ROOT}/scripts/sweep_full.sh" --help > /dev/null 2> "$HELP_FULL_OUT" || true
if grep -q "GNFS Performance Sweep" "$HELP_FULL_OUT"; then
    echo "  [PASS] sweep_full.sh --help renders banner"
    PASS=$(( PASS + 1 ))
else
    echo "  [FAIL] sweep_full.sh --help missing banner (zsh \$0-in-func bug?)"
    FAIL=$(( FAIL + 1 ))
    FAILED_TESTS+=("sweep_full --help")
fi
HELP_COMBO_OUT="$TMPDIR_BASE/help_combo.out"
"${PROJECT_ROOT}/scripts/sweep_combo.sh" --help > /dev/null 2> "$HELP_COMBO_OUT" || true
if grep -q "pairwise ENV combo" "$HELP_COMBO_OUT"; then
    echo "  [PASS] sweep_combo.sh --help renders banner"
    PASS=$(( PASS + 1 ))
else
    echo "  [FAIL] sweep_combo.sh --help missing banner"
    FAIL=$(( FAIL + 1 ))
    FAILED_TESTS+=("sweep_combo --help")
fi

echo
echo "=== sweep_analyze.py parsing smoke ==="
if [[ -f "$SWEEP_OUT" ]]; then
    run_check "analyzer parses sweep markdown" \
        assert_exit_zero python3 "${PROJECT_ROOT}/scripts/sweep_analyze.py" \
        "$SWEEP_OUT"
    run_check "analyzer JSON mode works" \
        assert_exit_zero python3 "${PROJECT_ROOT}/scripts/sweep_analyze.py" \
        --json "$SWEEP_OUT"
else
    echo "  [SKIP] analyzer test (sweep markdown missing)"
fi

echo
echo "=================================================="
echo "  Sweep smoke results: ${PASS} pass / ${FAIL} fail"
echo "=================================================="
if (( FAIL > 0 )); then
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  - $t"
    done
    exit 1
fi
exit 0
