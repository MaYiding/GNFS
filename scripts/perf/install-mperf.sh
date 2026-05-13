#!/usr/bin/env zsh
# scripts/perf/install-mperf.sh
# Clone, build, and install mperf (Apple Silicon hardware perf counters CLI)
# into ~/.local/bin (no /usr/local pollution, no brew conflict).
#
# Usage:
#   scripts/perf/install-mperf.sh           # first install
#   scripts/perf/install-mperf.sh --update  # pull + rebuild
#
# Requires: Xcode CLT (make, xcrun), git. mperf itself needs sudo at runtime.
#
# See docs/superpowers/plans/2026-05-13-pmu-events-deepening.md Task 1.

set -euo pipefail

MPERF_REPO="https://github.com/tmcgilchrist/mperf.git"
MPERF_DIR="${HOME}/.cache/mperf-src"
INSTALL_DIR="${HOME}/.local/bin"
UPDATE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --update) UPDATE=1; shift ;;
        -h|--help)
            awk '/^set -euo/{exit} NR>=2 && /^#/{sub(/^# ?/,""); print}' "$0"
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "${INSTALL_DIR}"
mkdir -p "$(dirname "${MPERF_DIR}")"

if [[ ! -d "${MPERF_DIR}" ]]; then
    echo "== Cloning mperf into ${MPERF_DIR} =="
    git clone "${MPERF_REPO}" "${MPERF_DIR}"
elif (( UPDATE )); then
    echo "== Updating mperf =="
    git -C "${MPERF_DIR}" pull --ff-only
fi

echo "== Building mperf =="
make -C "${MPERF_DIR}" clean >/dev/null 2>&1 || true
make -C "${MPERF_DIR}"

# mperf project may produce either ./mperf-stat or ./mperf depending on Makefile;
# probe both
BIN_SRC=""
for cand in "${MPERF_DIR}/mperf-stat" "${MPERF_DIR}/mperf"; do
    if [[ -x "${cand}" ]]; then
        BIN_SRC="${cand}"
        break
    fi
done
if [[ -z "${BIN_SRC}" ]]; then
    echo "ERROR: mperf binary not produced in ${MPERF_DIR}" >&2
    echo "Built files:" >&2
    ls -la "${MPERF_DIR}" >&2
    exit 1
fi

# Use the same basename as the source to keep callers in sync
BIN_NAME="$(basename "${BIN_SRC}")"
ln -sf "${BIN_SRC}" "${INSTALL_DIR}/${BIN_NAME}"

echo ""
echo "== Done =="
echo "Installed:  ${INSTALL_DIR}/${BIN_NAME} -> ${BIN_SRC}"
echo ""
echo "Add to PATH if needed:  export PATH=\"\${HOME}/.local/bin:\${PATH}\""
echo "Test:                   sudo ${INSTALL_DIR}/${BIN_NAME} -- /bin/echo hi"
