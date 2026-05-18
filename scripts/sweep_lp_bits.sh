#!/usr/bin/env zsh
# sweep_lp_bits.sh — empirical lp_bits comparison for GNFS at given digit count.
#
# BACKLOG #5: 60d lp_bits 25 vs 26 trade-off. Smaller lp_bits = smaller LP space
# = fewer LP cols = less raw needed for PASS, but fewer LP cofactor candidates
# (lower yield per sieve special-q). Net wall-time direction is empirical.
#
# This script launches one test_stress run per lp_bits value in the requested
# range, each in its own nohup background process logging to a separate file.
# After all runs complete, run sweep_lp_bits_report.sh (see below) to compare.
#
# Usage:
#   scripts/sweep_lp_bits.sh                       # default: 60d, lp_bits 25..26
#   scripts/sweep_lp_bits.sh 60 24 26              # 60d, lp_bits 24..26 (3 runs)
#   scripts/sweep_lp_bits.sh 60 25 25 /tmp/lp25    # single lp_bits=25 with custom outdir
#
# Each lp_bits run is multi-hour. Concurrent runs assume sufficient RAM (60d each
# needs 2-3 GiB peak with OOC). Reduce GNFS_SIEVE_ECORE_THREADS if thrashing.

set -euo pipefail

DIGITS="${1:-60}"
START_LP="${2:-25}"
END_LP="${3:-26}"
OUTDIR="${4:-/tmp/lp_sweep_$(date +%y%m%d_%H%M%S)}"

if [[ "$DIGITS" -lt 30 ]]; then
    echo "ERROR: lp_bits override below 30d not meaningful (params.hpp default lp_bits<22)"
    exit 1
fi
if [[ "$START_LP" -lt 1 || "$END_LP" -gt 30 || "$START_LP" -gt "$END_LP" ]]; then
    echo "ERROR: lp_bits range [$START_LP, $END_LP] outside valid [1, 30]"
    exit 1
fi

# Pick test_stress stage by digit count
case "$DIGITS" in
    50) STRESS_ARGS="1 1" ;;
    60) STRESS_ARGS="2 2" ;;
    *)
        echo "ERROR: only 50/60-digit supported via test_stress; got $DIGITS"
        exit 1
        ;;
esac

mkdir -p "$OUTDIR"
BG_TASKS="${BG_TASKS:-/tmp/bg_tasks.txt}"

if [[ ! -x ./build/test_stress ]]; then
    echo "ERROR: ./build/test_stress not found — run 'make -C build test_stress' first"
    exit 1
fi

echo "═══ GNFS lp_bits sweep ═══"
echo "  digits     = $DIGITS"
echo "  lp_bits    = $START_LP..$END_LP ($((END_LP - START_LP + 1)) runs)"
echo "  outdir     = $OUTDIR"
echo "  stress args= $STRESS_ARGS"
echo

for lp in $(seq "$START_LP" "$END_LP"); do
    LOG="$OUTDIR/lp${lp}_d${DIGITS}.log"
    echo "[$(date +%H:%M:%S)] launch lp_bits=$lp → $LOG"

    # Recommended baseline: OOC + V0_BFS default-ON (50d+/60d defaults).
    # Override lp_bits via GNFS_OVERRIDE_LP_BITS.
    GNFS_OVERRIDE_LP_BITS="$lp" \
    nohup ./build/test_stress $(echo "$STRESS_ARGS") > "$LOG" 2>&1 &
    pid=$!
    echo "  PID=$pid LP_BITS=$lp"
    echo "PID=$pid LOG=$LOG STARTED=$(date) PURPOSE=BACKLOG#5 lp_bits sweep ($DIGITS-digit lp_bits=$lp)" >> "$BG_TASKS"
done

echo
echo "All $((END_LP - START_LP + 1)) sweeps launched in background."
echo "Monitor progress:"
echo "  for log in $OUTDIR/*.log; do echo \"--- \$log ---\"; tail -3 \"\$log\"; done"
echo
echo "Compare wall-time after completion:"
echo "  for log in $OUTDIR/*.log; do"
echo "      echo -n \"\$(basename \$log): \"; grep -E 'PASS|FAIL|Total time' \"\$log\" | tail -2"
echo "  done"
echo
echo "Recommendation: PICK lp_bits with shortest wall-time AND PASS state. If both"
echo "PASS, the smaller lp_bits typically has lower memory peak too (smaller LP space)."
