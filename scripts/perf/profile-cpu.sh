#!/usr/bin/env zsh
# scripts/perf/profile-cpu.sh
# Record CPU PMU counters for a binary using xctrace + Instruments "CPU Counters" template
# (which on M-series runs in "CPU Bottlenecks" / TMA-style aggregation mode).
#
# Outputs:
#   bench/results/<YYYY-MM-DD-HHMMSS>-<basename>.trace        (binary trace, ~MB-GB)
#   bench/results/<YYYY-MM-DD-HHMMSS>-<basename>.trace.xml    (small, only counter rows)
#   bench/results/<YYYY-MM-DD-HHMMSS>-<basename>.trace.toc.xml (ToC for schema discovery)
#
# Usage:
#   scripts/perf/profile-cpu.sh <path-to-binary> [args...]
#   scripts/perf/profile-cpu.sh --template <name> <bin> [args...]   # override template
#
# Then:
#   python3 scripts/perf/parse-trace.py bench/results/<file>.trace.xml
#
# See docs/perf/performance-doctrine.md §5.4.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_DIR="${ROOT}/bench/results"

TEMPLATE="CPU Counters"
# 主要 schema: M5 CPU Bottlenecks 模式聚合的 4 维度 TMA + Cycles
# 参考 trace ToC 中 metricLegend:
#   index 0: Cycles
#   index 1: Instruction Delivery Bottleneck
#   index 2: Discarded Bottleneck
#   index 3: Instruction Processing Bottleneck
#   index 4: Useful
SCHEMA="CounterMetricAggregatedForSystem"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --template) TEMPLATE="$2"; shift 2 ;;
        --schema)   SCHEMA="$2"; shift 2 ;;
        -h|--help)
            awk '/^set -euo/{exit} NR>=2 && /^#/{sub(/^# ?/,""); print}' "$0"
            exit 0 ;;
        *) break ;;
    esac
done

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--template <name>] [--schema <schema>] <path-to-binary> [args...]" >&2
    exit 1
fi

BIN="$1"
shift
if [[ ! -x "${BIN}" ]]; then
    echo "ERROR: not an executable file: ${BIN}" >&2
    exit 1
fi

if ! command -v xctrace >/dev/null 2>&1; then
    echo "ERROR: xctrace not in PATH. Install Xcode Command Line Tools." >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"
TS=$(date +%Y-%m-%d-%H%M%S)
BASENAME=$(basename "${BIN}")
TRACE_OUT="${RESULTS_DIR}/${TS}-${BASENAME}.trace"
XML_OUT="${TRACE_OUT}.xml"
TOC_OUT="${TRACE_OUT}.toc.xml"

echo "== Recording trace =="
echo "  binary:   ${BIN} $*"
echo "  template: ${TEMPLATE}"
echo "  trace:    ${TRACE_OUT}"
xctrace record \
    --template "${TEMPLATE}" \
    --launch "${BIN}" \
    --output "${TRACE_OUT}" \
    -- "$@"

echo ""
echo "== Exporting ToC (schema discovery) =="
xctrace export --input "${TRACE_OUT}" --toc --output "${TOC_OUT}" 2>&1 | tail -2 || true
echo "  toc: ${TOC_OUT} ($(ls -lh "${TOC_OUT}" 2>/dev/null | awk '{print $5}'))"

echo ""
echo "== Exporting counter rows (schema: ${SCHEMA}) =="
xctrace export \
    --input "${TRACE_OUT}" \
    --xpath "/trace-toc/run/data/table[@schema=\"${SCHEMA}\"]" \
    --output "${XML_OUT}" 2>&1 | tail -2 || true

# Verify row count
n_rows=$(grep -c '<row' "${XML_OUT}" 2>/dev/null || echo 0)
echo "  -- captured ${n_rows} <row> samples in $(ls -lh "${XML_OUT}" 2>/dev/null | awk '{print $5}')"

if [[ "${n_rows}" -eq 0 ]]; then
    echo ""
    echo "WARN: zero rows captured. Available schemas in this trace:" >&2
    grep -oE 'schema="[^"]+"' "${TOC_OUT}" | sort -u | sed 's/^/  /' >&2
    echo "Try: $0 --schema <name> ${BIN}" >&2
fi

echo ""
echo "== Done =="
echo "Trace: ${TRACE_OUT}"
echo "XML:   ${XML_OUT}"
echo ""
echo "Parse with: python3 ${ROOT}/scripts/perf/parse-trace.py ${XML_OUT}"
