#!/usr/bin/env zsh
# ╔══════════════════════════════════════════════════════════════════╗
# ║  GNFS Performance Sweep — pairwise ENV combo (2×2 = 4 runs)       ║
# ║  Compare A=0 B=0 vs A=1 B=0 vs A=0 B=1 vs A=1 B=1                 ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# Useful when you suspect two ENV-gated features may interact (additive,
# synergistic, or canceling). Exhaustively walks the 2×2 grid over a
# specified value pair from each ENV. For ENVs with non-binary values pass
# the value via `--val-a` / `--val-b` (otherwise defaults to "1").
#
# ────────────────────────── Usage ──────────────────────────
#
#   ./scripts/sweep_combo.sh --bit 81 --env-a GNFS_POLY_BAI_BRENT \
#                            --env-b GNFS_ECM_BRENT_SUYAMA
#
#   ./scripts/sweep_combo.sh --bit 40 \
#       --env-a GNFS_BW_KRYLOV_STREAMS --val-a 4 \
#       --env-b GNFS_BW_KRYLOV_MMAP --val-b 1
#
#   ./scripts/sweep_combo.sh --bit 40 ... --dry-run
#
# ────────────────────────── Output ──────────────────────────
#
# Default file: bench/results/combo_<YYYYMMDD-HHMMSS>.md
# Sections:
#   1. Metadata (host, date, ENV-A/B, values)
#   2. 2×2 grid (rows = A off/on, cols = B off/on)
#   3. Interpretation (additive / synergy / cancel)

set -eo pipefail

zmodload zsh/datetime 2>/dev/null || true

PROJECT_ROOT="${0:A:h:h}"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULTS_DIR="${PROJECT_ROOT}/bench/results"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

typeset -a BIT_SIZES DIGIT_SIZES
BIT_SIZES=()
DIGIT_SIZES=()
ENV_A=""
ENV_B=""
VAL_A="1"
VAL_B="1"
OUTPUT_FILE=""
TIMEOUT_SEC=0
DRY_RUN=0
SKIP_BUILD=0
BUILD_TYPE="Debug"
USE_COLOR=1
QUIET=0

setup_colors() {
    if [[ -t 1 ]] && (( USE_COLOR )); then
        RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'
        BLUE=$'\033[0;34m'; CYAN=$'\033[0;36m'; MAGENTA=$'\033[0;35m'
        BOLD=$'\033[1m'; DIM=$'\033[2m'; RESET=$'\033[0m'
        CHECK='✓'; CROSS='✗'; WARN='⚠'; ARROW='→'
    else
        RED='' GREEN='' YELLOW='' BLUE='' CYAN='' MAGENTA='' BOLD='' DIM='' RESET=''
        CHECK='[PASS]'; CROSS='[FAIL]'; WARN='[WARN]'; ARROW='->'
    fi
}

log_info()    { (( QUIET )) || echo "${CYAN}${ARROW}${RESET} $*"; }
log_step()    { (( QUIET )) || echo "${BLUE}${BOLD}== $* ==${RESET}"; }
log_warn()    {               echo "${YELLOW}${WARN}${RESET} $*" >&2; }
log_error()   {               echo "${RED}${CROSS}${RESET} $*" >&2; }
log_success() { (( QUIET )) || echo "${GREEN}${CHECK}${RESET} $*"; }

print_help() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 0
}

while (( $# > 0 )); do
    case "$1" in
        --bit)        BIT_SIZES+=("$2"); shift 2 ;;
        --digit)      DIGIT_SIZES+=("$2"); shift 2 ;;
        --env-a)      ENV_A="$2"; shift 2 ;;
        --env-b)      ENV_B="$2"; shift 2 ;;
        --val-a)      VAL_A="$2"; shift 2 ;;
        --val-b)      VAL_B="$2"; shift 2 ;;
        --output|-o)  OUTPUT_FILE="$2"; shift 2 ;;
        --timeout|-t) TIMEOUT_SEC="$2"; shift 2 ;;
        --dry-run|-n) DRY_RUN=1; shift ;;
        --no-build)   SKIP_BUILD=1; shift ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --no-color)   USE_COLOR=0; shift ;;
        --quiet|-q)   QUIET=1; shift ;;
        --help|-h)    print_help ;;
        *)            log_error "Unknown option: $1"; exit 2 ;;
    esac
done

setup_colors

[[ -z "$ENV_A" ]] && { log_error "--env-a required"; exit 2; }
[[ -z "$ENV_B" ]] && { log_error "--env-b required"; exit 2; }
if (( ${#BIT_SIZES} == 0 && ${#DIGIT_SIZES} == 0 )); then
    BIT_SIZES=(40)
fi

typeset -A TEST_N
TEST_N=(
    bit_27   "100160063"
    bit_40   "1000036000099"
    bit_50   "100000980001501"
    bit_81   "1669994516749619561652133"
)

typeset -A SIZE_TIMEOUT_DEFAULT
SIZE_TIMEOUT_DEFAULT=(
    bit_27    30
    bit_40    60
    bit_50    90
    bit_81    180
    digit_50  10800
    digit_60  21600
)

ensure_build() {
    (( SKIP_BUILD )) && return 0
    log_step "Building ($BUILD_TYPE)"
    if [[ ! -d "$BUILD_DIR" ]]; then
        cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null
    fi
    make -C "$BUILD_DIR" -j"$NCPU" gnfs test_stress >/dev/null 2>&1 \
        || { log_error "Build failed; rerun with --no-build after fixing"; exit 3; }
    log_success "Build OK"
}

resolve_command() {
    local size_key="$1"
    local timeout_override="$2"
    RESOLVED_CMD=""
    RESOLVED_TIMEOUT=""
    RESOLVED_N_LABEL=""

    case "$size_key" in
        bit_*)
            local n="${TEST_N[$size_key]}"
            [[ -z "$n" ]] && { log_error "Unknown bit size: $size_key"; return 1; }
            RESOLVED_CMD="env GNFS_DISABLE_SIQS=1 ./build/gnfs $n --method gnfs --quiet"
            RESOLVED_N_LABEL="N=${n}"
            ;;
        digit_50)
            RESOLVED_CMD="./build/test_stress 1 1"
            RESOLVED_N_LABEL="50-digit (test_stress L1)"
            ;;
        digit_60)
            RESOLVED_CMD="./build/test_stress 2 2"
            RESOLVED_N_LABEL="60-digit (test_stress L2)"
            ;;
        *)
            log_error "Unknown size key: $size_key"
            return 1
            ;;
    esac

    if [[ -n "$timeout_override" && "$timeout_override" != 0 ]]; then
        RESOLVED_TIMEOUT="$timeout_override"
    else
        RESOLVED_TIMEOUT="${SIZE_TIMEOUT_DEFAULT[$size_key]:-300}"
    fi
}

run_with_timeout() {
    local cmd="$1"
    local env_assignments="$2"
    local timeout_sec="$3"
    local log_file="$4"

    RUN_ELAPSED_MS=0
    RUN_STATUS="UNKNOWN"

    local start_s
    if [[ -n "$EPOCHREALTIME" ]]; then
        start_s="$EPOCHREALTIME"
    elif (( ${+commands[gdate]} )); then
        start_s=$(gdate +%s.%N)
    else
        start_s=$(date +%s)
    fi

    local full_cmd
    if [[ -n "$env_assignments" ]]; then
        full_cmd="env $env_assignments $cmd"
    else
        full_cmd="$cmd"
    fi

    rm -f "${log_file}.done"
    (
        set +e
        eval "$full_cmd" > "$log_file" 2>&1
        local ec=$?
        echo "EXIT_CODE=$ec" >> "$log_file"
        touch "${log_file}.done"
    ) &
    local run_pid=$!

    local elapsed=0
    while (( elapsed < timeout_sec )); do
        if [[ -f "${log_file}.done" ]]; then
            break
        fi
        sleep 1
        elapsed=$(( elapsed + 1 ))
    done

    if [[ ! -f "${log_file}.done" ]]; then
        kill -TERM "$run_pid" 2>/dev/null || true
        local k=0
        while (( k < 2 )); do
            sleep 1
            k=$(( k + 1 ))
            [[ -f "${log_file}.done" ]] && break
        done
        if [[ ! -f "${log_file}.done" ]]; then
            kill -KILL "$run_pid" 2>/dev/null || true
            sleep 1
            [[ ! -f "${log_file}.done" ]] && echo "WATCHDOG_TIMEOUT=1" >> "$log_file"
        fi
    fi

    local end_s
    if [[ -n "$EPOCHREALTIME" ]]; then
        end_s="$EPOCHREALTIME"
    elif (( ${+commands[gdate]} )); then
        end_s=$(gdate +%s.%N)
    else
        end_s=$(date +%s)
    fi
    RUN_ELAPSED_MS=$(awk -v s="$start_s" -v e="$end_s" \
        'BEGIN { printf "%d", (e - s) * 1000 }')

    local actual_exit=1
    if grep -q "^EXIT_CODE=" "$log_file" 2>/dev/null; then
        local ec_line
        ec_line=$(grep "^EXIT_CODE=" "$log_file" | tail -1)
        actual_exit="${ec_line#EXIT_CODE=}"
    fi

    rm -f "${log_file}.done"
    wait "$run_pid" 2>/dev/null || true

    if grep -q "^WATCHDOG_TIMEOUT=1" "$log_file" 2>/dev/null; then
        RUN_STATUS="TIMEOUT"
    elif [[ "$actual_exit" == "0" ]]; then
        RUN_STATUS="PASS"
    else
        RUN_STATUS="FAIL"
    fi
}

fmt_ms() {
    local ms="$1"
    if (( ms < 0 )); then
        echo "-"
    elif (( ms >= 60000 )); then
        awk -v m="$ms" 'BEGIN { printf "%.1fm", m / 60000.0 }'
    elif (( ms >= 1000 )); then
        awk -v m="$ms" 'BEGIN { printf "%.2fs", m / 1000.0 }'
    else
        echo "${ms}ms"
    fi
}

delta_pct() {
    local baseline_ms="$1"
    local current_ms="$2"
    if [[ -z "$baseline_ms" || "$baseline_ms" -eq 0 ]]; then
        echo "-"
        return
    fi
    awk -v b="$baseline_ms" -v c="$current_ms" \
        'BEGIN { printf "%+.1f%%", (c - b) * 100.0 / b }'
}

# Globals populated per cell run.
typeset -A CELL_MS CELL_STATUS

run_combo_for_size() {
    local size_key="$1"
    local timeout_to_use="$2"

    resolve_command "$size_key" "$timeout_to_use" || return 1
    local cmd="$RESOLVED_CMD"
    local timeout="$RESOLVED_TIMEOUT"

    log_step "Size: $size_key ($RESOLVED_N_LABEL, per-run timeout ${timeout}s)"

    local tmpdir="${TMPDIR:-/tmp}/gnfs_combo_$$_$size_key"
    mkdir -p "$tmpdir"

    # 2x2 cells: A=off/on × B=off/on.
    for a_state in off on; do
        for b_state in off on; do
            local env_str=""
            local cell="${a_state}-${b_state}"
            if [[ "$a_state" == "on" ]]; then env_str+="${ENV_A}=${VAL_A} "; fi
            if [[ "$b_state" == "on" ]]; then env_str+="${ENV_B}=${VAL_B} "; fi
            env_str="${env_str%% }"

            log_info "  [${cell}] ENV={${env_str:-clean}} $cmd"
            if (( DRY_RUN )); then
                echo "DRY: env ${env_str:-(clean)} $cmd  # timeout ${timeout}s"
                CELL_MS[$cell]=0
                CELL_STATUS[$cell]="DRY"
                continue
            fi
            local log_path="$tmpdir/${cell}.log"
            run_with_timeout "$cmd" "$env_str" "$timeout" "$log_path"
            log_info "    ${RUN_STATUS} in $(fmt_ms "$RUN_ELAPSED_MS")"
            CELL_MS[$cell]="$RUN_ELAPSED_MS"
            CELL_STATUS[$cell]="$RUN_STATUS"
        done
    done
}

render_markdown() {
    local outfile="$1"
    local size_label="$2"

    local off_off="${CELL_MS[off-off]:--1}"
    local off_on="${CELL_MS[off-on]:--1}"
    local on_off="${CELL_MS[on-off]:--1}"
    local on_on="${CELL_MS[on-on]:--1}"

    local off_off_s="${CELL_STATUS[off-off]:-?}"
    local off_on_s="${CELL_STATUS[off-on]:-?}"
    local on_off_s="${CELL_STATUS[on-off]:-?}"
    local on_on_s="${CELL_STATUS[on-on]:-?}"

    {
        echo "## Combo: ${size_label}"
        echo
        echo "ENV-A: \`${ENV_A}=${VAL_A}\`  ENV-B: \`${ENV_B}=${VAL_B}\`"
        echo
        echo "| A \\ B | B = off | B = on |"
        echo "|-------|---------|--------|"
        local row_off="| **A = off** | $(fmt_ms "$off_off") (${off_off_s})"
        local da="—"
        if [[ "$off_off_s" == "PASS" && "$off_on_s" == "PASS" ]]; then
            da=$(delta_pct "$off_off" "$off_on")
        fi
        row_off+=" | $(fmt_ms "$off_on") (${off_on_s}) Δ${da} |"
        echo "$row_off"

        local row_on="| **A = on**"
        local dA="—"
        if [[ "$off_off_s" == "PASS" && "$on_off_s" == "PASS" ]]; then
            dA=$(delta_pct "$off_off" "$on_off")
        fi
        row_on+=" | $(fmt_ms "$on_off") (${on_off_s}) Δ${dA}"
        local dAB="—"
        if [[ "$off_off_s" == "PASS" && "$on_on_s" == "PASS" ]]; then
            dAB=$(delta_pct "$off_off" "$on_on")
        fi
        row_on+=" | $(fmt_ms "$on_on") (${on_on_s}) Δ${dAB} |"
        echo "$row_on"

        echo
        # Interpretation
        if [[ "$off_off_s" == "PASS" && "$on_off_s" == "PASS" \
              && "$off_on_s" == "PASS" && "$on_on_s" == "PASS" ]]; then
            # Compute expected combined effect (additive) and observed effect.
            local expected_combined observed_combined
            expected_combined=$(awk -v b="$off_off" -v ao="$on_off" -v bo="$off_on" \
                'BEGIN { printf "%.0f", ao + bo - b }')
            observed_combined="$on_on"
            local diff_pct
            diff_pct=$(awk -v e="$expected_combined" -v o="$observed_combined" \
                'BEGIN {
                    if (e == 0) { print "n/a"; exit }
                    printf "%+.1f%%", (o - e) * 100.0 / e
                }')
            echo "### Interpretation"
            echo
            echo "- Additive prediction: ${expected_combined}ms (sum of independent deltas)"
            echo "- Observed combined: ${observed_combined}ms"
            echo "- Synergy / cancel vs additive: **${diff_pct}**"
            echo "  - Negative (combined faster than additive) suggests synergy"
            echo "  - Positive (combined slower than additive) suggests cancel / contention"
        else
            echo "### Interpretation"
            echo
            echo "At least one cell did not PASS; cannot derive additive comparison."
        fi
        echo
    } >> "$outfile"
}

main() {
    ensure_build

    mkdir -p "$RESULTS_DIR"
    if [[ -z "$OUTPUT_FILE" ]]; then
        OUTPUT_FILE="${RESULTS_DIR}/combo_$(date +%Y%m%d-%H%M%S).md"
    fi

    {
        echo "# GNFS Combo Sweep — $(date '+%Y-%m-%d %H:%M:%S')"
        echo
        echo "| Key | Value |"
        echo "|-----|-------|"
        echo "| Host | $(hostname) |"
        echo "| Git HEAD | $(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown) |"
        echo "| ENV-A | \`${ENV_A}=${VAL_A}\` |"
        echo "| ENV-B | \`${ENV_B}=${VAL_B}\` |"
        echo "| Sizes | ${BIT_SIZES[*]:-(none)} bits / ${DIGIT_SIZES[*]:-(none)} digits |"
        echo
    } > "$OUTPUT_FILE"

    log_step "Combo configuration"
    log_info "  ENV-A: ${ENV_A} = ${VAL_A}"
    log_info "  ENV-B: ${ENV_B} = ${VAL_B}"
    log_info "  Sizes: bits=${BIT_SIZES[*]:-(none)}, digits=${DIGIT_SIZES[*]:-(none)}"

    for b in "${BIT_SIZES[@]}"; do
        unset CELL_MS CELL_STATUS
        typeset -gA CELL_MS CELL_STATUS
        run_combo_for_size "bit_$b" "$TIMEOUT_SEC"
        render_markdown "$OUTPUT_FILE" "${b}-bit (N=${TEST_N[bit_$b]})"
    done
    for d in "${DIGIT_SIZES[@]}"; do
        unset CELL_MS CELL_STATUS
        typeset -gA CELL_MS CELL_STATUS
        run_combo_for_size "digit_$d" "$TIMEOUT_SEC"
        render_markdown "$OUTPUT_FILE" "${d}-digit"
    done

    log_success "Report written: $OUTPUT_FILE"
}

main "$@"
