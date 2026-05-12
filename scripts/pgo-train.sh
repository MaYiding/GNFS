#!/usr/bin/env zsh
# scripts/pgo-train.sh
# Automate Clang PGO (Profile-Guided Optimization) four-phase workflow:
#   1. Instrumented build  (build-pgo-gen/)
#   2. Training runs       (generate .profraw)
#   3. Profile merge       (llvm-profdata merge -> merged.profdata)
#   4. Optimized build     (build-pgo-use/)
#
# Usage:
#   scripts/pgo-train.sh                  # default training samples
#   scripts/pgo-train.sh --keep            # keep .profraw files after merge
#   scripts/pgo-train.sh --clean           # wipe build-pgo-{gen,use} first
#
# See docs/perf/performance-doctrine.md §5.3 for design rationale.

set -euo pipefail

# zsh datetime module for sub-second timing (EPOCHREALTIME)
zmodload zsh/datetime

# Resolve project root (scripts/ -> ..)
SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_GEN="${ROOT}/build-pgo-gen"
BUILD_USE="${ROOT}/build-pgo-use"
PROFILE_DIR="${BUILD_GEN}/pgo-profiles"

KEEP_PROFRAW=0
CLEAN_FIRST=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep)  KEEP_PROFRAW=1; shift ;;
        --clean) CLEAN_FIRST=1; shift ;;
        -h|--help)
            awk '/^set -euo/{exit} NR>=2 && /^#/{sub(/^# ?/,""); print}' "$0"
            exit 0 ;;
        *)
            echo "Unknown arg: $1" >&2
            exit 1 ;;
    esac
done

JOBS=$(sysctl -n hw.ncpu)

if (( CLEAN_FIRST )); then
    echo "== Cleaning previous PGO build directories =="
    rm -rf "${BUILD_GEN}" "${BUILD_USE}"
fi

# ----- Sanity checks -----
if ! command -v xcrun >/dev/null 2>&1; then
    echo "ERROR: xcrun not found; install Xcode Command Line Tools" >&2
    exit 1
fi
if ! xcrun llvm-profdata --version >/dev/null 2>&1; then
    echo "ERROR: xcrun llvm-profdata not available" >&2
    exit 1
fi

echo ""
echo "==================== Phase 1/4: Instrumented build ===================="
mkdir -p "${BUILD_GEN}"
cmake -B "${BUILD_GEN}" -S "${ROOT}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_GEN=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}" \
    > "${BUILD_GEN}/cmake-config.log" 2>&1 || {
        echo "CMake configure failed; see ${BUILD_GEN}/cmake-config.log" >&2
        tail -20 "${BUILD_GEN}/cmake-config.log" >&2
        exit 1
    }
echo "  -- configure ok"

# Only build the training-sample binaries (not all 41 tests; save time)
cmake --build "${BUILD_GEN}" -j"${JOBS}" \
    --target test_factor_with_kleinjung test_lattice_sieve test_linalg \
    > "${BUILD_GEN}/cmake-build.log" 2>&1 || {
        echo "Build failed; see ${BUILD_GEN}/cmake-build.log" >&2
        tail -30 "${BUILD_GEN}/cmake-build.log" >&2
        exit 1
    }
echo "  -- build ok (3 training targets)"

echo ""
echo "==================== Phase 2/4: Training runs ===================="
mkdir -p "${PROFILE_DIR}"
# Module-pid pattern: each invocation gets a unique .profraw
export LLVM_PROFILE_FILE="${PROFILE_DIR}/%m-%p.profraw"

run_training() {
    local name="$1"
    local bin="${BUILD_GEN}/${name}"
    if [[ ! -x "${bin}" ]]; then
        echo "ERROR: training binary not built: ${bin}" >&2
        exit 1
    fi
    echo "  -- training: ${name}"
    local t0=$EPOCHREALTIME
    "${bin}" > "${BUILD_GEN}/${name}.train.log" 2>&1 || {
        echo "ERROR: training run failed for ${name}; see ${BUILD_GEN}/${name}.train.log" >&2
        tail -20 "${BUILD_GEN}/${name}.train.log" >&2
        exit 1
    }
    local t1=$EPOCHREALTIME
    printf "     elapsed: %.1fs\n" $(( t1 - t0 ))
}

run_training test_factor_with_kleinjung
run_training test_lattice_sieve
run_training test_linalg

n_profraw=$(ls -1 "${PROFILE_DIR}"/*.profraw 2>/dev/null | wc -l | tr -d ' ')
echo "  -- collected ${n_profraw} .profraw files in ${PROFILE_DIR}"
if [[ "${n_profraw}" -eq 0 ]]; then
    echo "ERROR: no .profraw produced. Check LLVM_PROFILE_FILE and PGO instrumentation." >&2
    exit 1
fi

echo ""
echo "==================== Phase 3/4: Merge profiles ===================="
xcrun llvm-profdata merge \
    -output="${PROFILE_DIR}/merged.profdata" \
    "${PROFILE_DIR}"/*.profraw
ls -lh "${PROFILE_DIR}/merged.profdata"
echo "  -- merge ok"

if (( KEEP_PROFRAW == 0 )); then
    rm -f "${PROFILE_DIR}"/*.profraw
    echo "  -- cleaned up .profraw (use --keep to retain)"
fi

echo ""
echo "==================== Phase 4/4: PGO-optimized build ===================="
mkdir -p "${BUILD_USE}"
cmake -B "${BUILD_USE}" -S "${ROOT}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_USE=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}" \
    > "${BUILD_USE}/cmake-config.log" 2>&1 || {
        echo "CMake configure failed; see ${BUILD_USE}/cmake-config.log" >&2
        tail -20 "${BUILD_USE}/cmake-config.log" >&2
        exit 1
    }
echo "  -- configure ok"

cmake --build "${BUILD_USE}" -j"${JOBS}" \
    > "${BUILD_USE}/cmake-build.log" 2>&1 || {
        echo "PGO-optimized build failed; see ${BUILD_USE}/cmake-build.log" >&2
        tail -30 "${BUILD_USE}/cmake-build.log" >&2
        exit 1
    }
echo "  -- build ok (full 41 test targets)"

echo ""
echo "==================== Done ===================="
echo "Baseline binaries:  ${ROOT}/build/test_*           (-O3 -flto, no PGO)"
echo "PGO binaries:       ${BUILD_USE}/test_*           (-O3 -flto -fprofile-instr-use)"
echo ""
echo "Quick wall-time comparison:"
echo "  time ${ROOT}/build/test_factor_with_kleinjung           # baseline"
echo "  time ${BUILD_USE}/test_factor_with_kleinjung            # PGO"
echo ""
echo "Full PMU diff (via Instruments + parse-trace.py):"
echo "  ./scripts/test.sh profile factor_with_kleinjung        # baseline"
echo "  ./scripts/perf/profile-cpu.sh ${BUILD_USE}/test_factor_with_kleinjung    # PGO"
echo "  python3 scripts/perf/parse-trace.py <baseline.xml> <pgo.xml>"
