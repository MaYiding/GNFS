#!/bin/bash
# V3 cascade ROI estimator — 给定 V0 baseline Round 数据估算 V3 cascade 是否值得 enable.
#
# 用法:
#   bash scripts/v3_roi_estimator.sh [<log_file>] [<beta>]
#
# 参数:
#   <log_file>  test_stress 或 GNFS pipeline 的 log 文件 (默认 /tmp/p3_v0fix_60d.log)
#   <beta>      LP cols / usable ratio (50d≈0.64, 60d≈0.70, 默认 0.70)
#
# 输出:
#   - Round-by-Round α 数据 (merge_rate)
#   - 推断 matrix_cols
#   - V0 vs V0+V3 needed raw 比较
#   - Wall time estimate
#   - Enable V3 cascade 建议
#
# 例:
#   bash scripts/v3_roi_estimator.sh /tmp/p3_v0fix_50d.log 0.64
#   bash scripts/v3_roi_estimator.sh /tmp/p3_v0fix_60d.log 0.70

LOG=${1:-/tmp/p3_v0fix_60d.log}
BETA=${2:-0.70}

if [[ ! -f "$LOG" ]]; then
    echo "ERROR: log file not found: $LOG"
    echo "Usage: $0 <log_file> [<beta>]"
    exit 1
fi

echo "═══════════════════════════════════════════════════"
echo " V3 Cascade ROI Estimator — $(date '+%H:%M:%S')"
echo "═══════════════════════════════════════════════════"
echo "Source: $LOG"
echo "β (lp_cols/usable): $BETA"
echo ""

echo "## Round-by-Round merge_rate (α):"
grep -E "merge_rate=[0-9.]+%" "$LOG" 2>/dev/null | \
    sed -n 's/.*merge_rate=\([0-9.]*%\).*new target=\([0-9]*\).*/  α=\1 target=\2/p' | head -10

echo ""
echo "## V3 Cascade ROI prediction:"

LAST_ALPHA=$(grep -E "merge_rate=" "$LOG" 2>/dev/null | tail -1 | \
    sed -n 's/.*merge_rate=\([0-9.]*\)%.*/\1/p')

if [[ -z "$LAST_ALPHA" ]]; then
    echo "  No filter data yet (sieve still in progress)"
    exit 0
fi

LAST_USABLE=$(grep -E "usable=[0-9]+/" "$LOG" 2>/dev/null | tail -1 | \
    sed -n 's/.*usable=\([0-9]*\)\/\([0-9]*\) merge_rate.*/\1/p')
LAST_EFFECTIVE=$(grep -E "usable=[0-9]+/" "$LOG" 2>/dev/null | tail -1 | \
    sed -n 's/.*usable=\([0-9]*\)\/\([0-9]*\) merge_rate.*/\2/p')

echo "  Latest data: α=${LAST_ALPHA}%, usable=$LAST_USABLE, effective_cols=$LAST_EFFECTIVE"

if [[ -n "$LAST_USABLE" && -n "$LAST_EFFECTIVE" ]]; then
    MATRIX_COLS=$(python3 -c "print(int($LAST_EFFECTIVE - $BETA * $LAST_USABLE))")
    echo "  Inferred matrix_cols: ~$MATRIX_COLS"

    V3_ALPHA_BOOST=1.06
    V3_ALPHA=$(python3 -c "print(round($LAST_ALPHA * $V3_ALPHA_BOOST, 3))")
    echo "  V3 cascade predicted α: ~${V3_ALPHA}% (+6% boost vs baseline)"

    NEED_RAW_V0=$(python3 -c "print(int($MATRIX_COLS / (1 - $BETA) / ($LAST_ALPHA / 100) * 1.1))")
    NEED_RAW_V3=$(python3 -c "print(int($MATRIX_COLS / (1 - $BETA) / ($V3_ALPHA / 100) * 1.1))")
    SAVINGS_PCT=$(python3 -c "print(round((1 - $NEED_RAW_V3 / $NEED_RAW_V0) * 100, 1))")
    echo ""
    echo "  Raw rels needed (PASS):"
    echo "    V0 only: ~$NEED_RAW_V0"
    echo "    V0+V3:   ~$NEED_RAW_V3"
    echo "    Savings: ${SAVINGS_PCT}%"

    # Sieve rate: 50d ≈ 290/s, 60d ≈ 80/s (defaults to 80 for conservative)
    SIEVE_RATE=${SIEVE_RATE:-80}
    TIME_V0_HRS=$(python3 -c "print(round($NEED_RAW_V0 / $SIEVE_RATE / 3600, 1))")
    TIME_V3_HRS=$(python3 -c "print(round($NEED_RAW_V3 / $SIEVE_RATE / 3600, 1))")
    echo ""
    echo "  Wall time estimate (sieve rate ${SIEVE_RATE}/s):"
    echo "    V0 only: ~${TIME_V0_HRS}h"
    echo "    V0+V3:   ~${TIME_V3_HRS}h"

    echo ""
    echo "## Recommendation:"
    if (( $(python3 -c "print(1 if $LAST_USABLE > $LAST_EFFECTIVE else 0)") )); then
        echo "  ✓ PASS already (current Round) — V3 cascade unnecessary"
    elif (( $(python3 -c "print(1 if $SAVINGS_PCT > 5 else 0)") )); then
        echo "  ▲ V3 cascade RECOMMENDED — saves ${SAVINGS_PCT}% sieve wall time"
        echo "    Enable: GNFS_CASCADE_V3=1 (force) or =auto (Round 2+ only)"
    else
        echo "  ▽ V3 cascade marginal — savings < 5%, V0 path simpler"
    fi
fi

echo ""
echo "═══════════════════════════════════════════════════"
