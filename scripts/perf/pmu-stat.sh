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

# GNFS P1.A event set — aligned with mperf-stat -l on M5 (as5.plist).
# Order matters: mperf assigns slots greedily, so events with restrictive
# counters_mask (slot constraints) MUST come first. INST_BRANCH and
# BRANCH_MISPRED_NONSPEC have mask=0b11111100 (slots 0/1 only); putting
# them after free events causes "Failed to add event (conflict: 0xfc)".
EVENTS=(
    FIXED_CYCLES                    # fixed slot 0
    FIXED_INSTRUCTIONS              # fixed slot 1
    INST_BRANCH                     # config: mask=252, MUST be early
    BRANCH_MISPRED_NONSPEC          # config: mask=252, MUST be early
    ARM_STALL_BACKEND               # config: no mask
    ARM_STALL_FRONTEND              # config: no mask
    L1D_CACHE_MISS_LD               # config: no mask
    L1D_TLB_MISS                    # config: no mask
    ARM_MEM_ACCESS                  # config: no mask
    MAP_SIMD_UOP                    # config: no mask
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
echo ""

# mperf needs kpc which requires root. If already root (e.g. user did
# `sudo bash` for a batch of runs), skip the inner sudo. Otherwise call
# sudo so the prompt reaches the user's terminal.
if [[ $EUID -eq 0 ]]; then
    "${MPERF}" -j "${ARGS[@]}" -- "${BIN}" "$@" > "${JSON_OUT}"
else
    echo "  (mperf needs sudo for kpc API; you may be prompted)"
    echo ""
    sudo "${MPERF}" -j "${ARGS[@]}" -- "${BIN}" "$@" > "${JSON_OUT}"
fi

echo ""
echo "== Done =="
echo "JSON: ${JSON_OUT} ($(ls -lh "${JSON_OUT}" | awk '{print $5}'))"
echo ""
echo "Parse: python3 ${SCRIPT_DIR}/pmu-derive.py ${JSON_OUT}"
