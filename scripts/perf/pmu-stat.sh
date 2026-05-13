#!/usr/bin/env zsh
# scripts/perf/pmu-stat.sh
# Run a target binary under mperf with the GNFS P1.A event set (10 slots),
# emit JSON to bench/results/<timestamp>-<basename>.pmu.json.
#
# Usage:
#   scripts/perf/pmu-stat.sh <path-to-binary> [args...]
#   scripts/perf/pmu-stat.sh --events "alt,set" <bin>  # override default events
#   scripts/perf/pmu-stat.sh --out <suffix> <bin>      # tag JSON name with suffix
#
# Requires: mperf-stat in PATH or ~/.local/bin/mperf-stat. Needs sudo (kpc API).
#
# See docs/superpowers/plans/2026-05-13-pmu-events-deepening.md Task 2.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_DIR="${ROOT}/bench/results"

# GNFS P1.A event set — aligned with mperf-stat -l on M5 (as5.plist)
EVENTS=(
    FIXED_CYCLES
    FIXED_INSTRUCTIONS
    ARM_STALL_BACKEND
    ARM_STALL_FRONTEND
    L1D_CACHE_MISS_LD
    L1D_TLB_MISS
    ARM_MEM_ACCESS
    BRANCH_MISPRED_NONSPEC
    INST_BRANCH
    MAP_SIMD_UOP
)

OUT_SUFFIX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --events) IFS=',' read -rA EVENTS <<< "$2"; shift 2 ;;
        --out)    OUT_SUFFIX="-$2"; shift 2 ;;
        -h|--help)
            awk '/^set -euo/{exit} NR>=2 && /^#/{sub(/^# ?/,""); print}' "$0"
            exit 0 ;;
        *) break ;;
    esac
done

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--events <comma-list>] [--out <suffix>] <bin> [args...]" >&2
    exit 1
fi

BIN="$1"; shift
[[ -x "${BIN}" ]] || { echo "ERROR: not executable: ${BIN}" >&2; exit 1; }

# Locate mperf-stat
MPERF=""
if command -v mperf-stat >/dev/null 2>&1; then
    MPERF=$(command -v mperf-stat)
elif [[ -x "${HOME}/.local/bin/mperf-stat" ]]; then
    MPERF="${HOME}/.local/bin/mperf-stat"
else
    echo "ERROR: mperf-stat not found. Run ./scripts/perf/install-mperf.sh first." >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"
TS=$(date +%Y-%m-%d-%H%M%S)
BASENAME=$(basename "${BIN}")
JSON_OUT="${RESULTS_DIR}/${TS}-${BASENAME}${OUT_SUFFIX}.pmu.json"

# Build -e args
ARGS=()
for ev in "${EVENTS[@]}"; do
    ARGS+=(-e "${ev}")
done

echo "== Recording PMU =="
echo "  binary:  ${BIN} $*"
echo "  events:  ${(j:,:)EVENTS}"
echo "  out:     ${JSON_OUT}"
echo "  (mperf needs sudo for kpc API; you may be prompted)"
echo ""

sudo "${MPERF}" -j "${ARGS[@]}" -- "${BIN}" "$@" > "${JSON_OUT}"

echo ""
echo "== Done =="
echo "JSON: ${JSON_OUT} ($(ls -lh "${JSON_OUT}" | awk '{print $5}'))"
echo ""
echo "Parse: python3 ${SCRIPT_DIR}/pmu-derive.py ${JSON_OUT}"
