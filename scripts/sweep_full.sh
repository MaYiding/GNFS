#!/usr/bin/env zsh
# ╔══════════════════════════════════════════════════════════════════╗
# ║  GNFS Performance Sweep — single-variable ENV auto-tuner          ║
# ║  Iterate over ENV values per N size, record wall-time, pick best  ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# Walk through every ENV in the sweep matrix one at a time (single-variable
# sweep, NOT exhaustive Cartesian product), comparing each value against a
# clean-environment baseline. Emit a markdown report under bench/results/
# with per-N tables plus a "best ENV value per size" recap.
#
# ────────────────────────── Usage ──────────────────────────
#
#   ./scripts/sweep_full.sh                      # default: 40+81 bit
#   ./scripts/sweep_full.sh --bit 40             # single bit size
#   ./scripts/sweep_full.sh --bit 40 --bit 81    # multiple bit sizes
#   ./scripts/sweep_full.sh --digit 50           # 50-digit (slow, hours)
#   ./scripts/sweep_full.sh --digit 60           # 60-digit (slow, hours)
#   ./scripts/sweep_full.sh --env-set "GNFS_POLY_BAI_BRENT GNFS_BW_KRYLOV_STREAMS"
#   ./scripts/sweep_full.sh --output bench/results/sweep_custom.md
#   ./scripts/sweep_full.sh --timeout 120        # per-run wall-time cap (s)
#   ./scripts/sweep_full.sh --dry-run            # print plan only, do not run
#   ./scripts/sweep_full.sh --no-build           # skip cmake/make step
#   ./scripts/sweep_full.sh --build-type Release # cmake build type override
#   ./scripts/sweep_full.sh --append             # append to existing report
#
# ────────────────────────── Output ──────────────────────────
#
# Default file: bench/results/sweep_<YYYYMMDD-HHMMSS>.md
# Sections:
#   1. Metadata (host, date, build type, N sizes, ENV set)
#   2. Per-N tables: |ENV|Value|Wall time|Status|Δ vs baseline|
#   3. Best ENV value per N size (winning configuration recap)
#
# ────────────────────────── ENV matrix ──────────────────────────
#
# Source of truth: CLAUDE.md "ENV-gated features" section.
# Modify SWEEP_MATRIX below to extend or restrict the ENV set.

set -eo pipefail

# zsh/datetime exposes $EPOCHREALTIME (microsecond float) so wall-time
# measurements have sub-second precision without requiring GNU coreutils.
zmodload zsh/datetime 2>/dev/null || true

PROJECT_ROOT="${0:A:h:h}"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULTS_DIR="${PROJECT_ROOT}/bench/results"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# ── Defaults ──
typeset -a BIT_SIZES DIGIT_SIZES ENV_FILTER
BIT_SIZES=()
DIGIT_SIZES=()
ENV_FILTER=()
OUTPUT_FILE=""
TIMEOUT_SEC=0   # 0 = auto per-size default
DRY_RUN=0
SKIP_BUILD=0
BUILD_TYPE="Debug"
APPEND_MODE=0
USE_COLOR=1
QUIET=0

# ── Color helpers (mirror test.sh style) ──
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

# ── CLI parsing ──
print_help() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 0
}

while (( $# > 0 )); do
    case "$1" in
        --bit)        BIT_SIZES+=("$2"); shift 2 ;;
        --digit)      DIGIT_SIZES+=("$2"); shift 2 ;;
        --env-set)    ENV_FILTER+=(${(s: :)2}); shift 2 ;;
        --output|-o)  OUTPUT_FILE="$2"; shift 2 ;;
        --timeout|-t) TIMEOUT_SEC="$2"; shift 2 ;;
        --dry-run|-n) DRY_RUN=1; shift ;;
        --no-build)   SKIP_BUILD=1; shift ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --append|-a)  APPEND_MODE=1; shift ;;
        --no-color)   USE_COLOR=0; shift ;;
        --quiet|-q)   QUIET=1; shift ;;
        --help|-h)    print_help ;;
        *)            log_error "Unknown option: $1"; exit 2 ;;
    esac
done

setup_colors

# ── Default sizes when none specified ──
if (( ${#BIT_SIZES} == 0 && ${#DIGIT_SIZES} == 0 )); then
    BIT_SIZES=(40 81)
fi

# ── Test N for each size (semiprime, validated) ──
# Each entry: N_<key>="<decimal>". Keys are bit/digit-tagged identifiers.
typeset -A TEST_N
TEST_N=(
    bit_27   "100160063"                                # 10007 * 10009
    bit_40   "1000036000099"                            # 1000003 * 1000033
    bit_50   "100000980001501"                          # 10000019 * 10000079
    bit_81   "1669994516749619561652133"                # 25-digit semiprime
)

# ── Per-size auto-timeout defaults (seconds) ──
typeset -A SIZE_TIMEOUT_DEFAULT
SIZE_TIMEOUT_DEFAULT=(
    bit_27    30
    bit_40    60
    bit_50    90
    bit_81    180
    digit_50  10800   # 3 h cap; nohup-mode honors test_stress 12 h internal
    digit_60  21600   # 6 h cap
)

# ── ENV sweep matrix ──
# Each entry: "<ENV_NAME>:<val1>,<val2>,..."
# Baseline (no ENV set) is implicitly the first comparison point.
#
# NOTE: Bit-size runs use `--method gnfs`, which overrides the SIQS-routing
# ENVs (`GNFS_FORCE_SIQS`, `GNFS_DISABLE_SIQS`). Those ENVs are therefore
# excluded from the bit-size default matrix to keep the sweep meaningful.
# Add them via `--env-set "GNFS_FORCE_SIQS …"` for a digit-size sweep
# (digit_50 / digit_60 do not pass `--method`).
typeset -a SWEEP_MATRIX
SWEEP_MATRIX=(
    "GNFS_POLY_BAI_BRENT:1"
    "GNFS_ECM_BRENT_SUYAMA:1"
    "GNFS_ECM_BS_DEGREE:12,30"
    "GNFS_BW_KRYLOV_MMAP:1"
    "GNFS_BW_KRYLOV_STREAMS:2,4"
    "GNFS_MURPHY_ALPHA_THREADS:0,8"
    "GNFS_OOC_RELATIONS:1"
    "GNFS_V0_BFS:1"
    "GNFS_V0_WEIGHT3:1"
    "GNFS_CASCADE_V3:auto,1"
    "GNFS_OVERRIDE_LP_BITS:20,22,24,26"
    "GNFS_SIEVE_ECORE_THREADS:0,4"
)

# ── ENV filter: keep only requested ENVs when --env-set is used ──
filter_matrix() {
    if (( ${#ENV_FILTER} == 0 )); then return 0; fi
    typeset -a kept
    kept=()
    for entry in "${SWEEP_MATRIX[@]}"; do
        local name="${entry%%:*}"
        for want in "${ENV_FILTER[@]}"; do
            if [[ "$name" == "$want" ]]; then
                kept+=("$entry")
                break
            fi
        done
    done
    SWEEP_MATRIX=("${kept[@]}")
}

# ── Build helpers ──
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

# ── Resolve command for a given size key ──
# Stores resolved bits in global RESOLVED_CMD, RESOLVED_TIMEOUT,
# RESOLVED_N_LABEL.
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
            # Force GNFS path so the sweep actually exercises sieve/linalg.
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

# ── Run a single command with timeout, return wall-time + status ──
# Outputs into globals: RUN_ELAPSED_MS, RUN_STATUS ("PASS"/"FAIL"/"TIMEOUT")
run_with_timeout() {
    local cmd="$1"
    local env_assignments="$2"     # space-separated KEY=VAL pairs (may be empty)
    local timeout_sec="$3"
    local log_file="$4"

    RUN_ELAPSED_MS=0
    RUN_STATUS="UNKNOWN"

    local start_s
    if [[ -n "$EPOCHREALTIME" ]]; then
        start_s="$EPOCHREALTIME"     # zsh/datetime float (µs precision)
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

    # File-flag based timeout: spawn run in background subshell that writes
    # EXIT_CODE and touches .done on completion. Poll the .done flag with a
    # 1 s sleep loop; kill on timeout. set +e inside the subshell so a
    # tested binary's non-zero exit does not kill the reaper before it
    # records the exit code.
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
        # Grace period for cooperative exit.
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
    # Use awk for the subtraction so we keep sub-second precision when
    # EPOCHREALTIME or gdate is available.
    RUN_ELAPSED_MS=$(awk -v s="$start_s" -v e="$end_s" \
        'BEGIN { printf "%d", (e - s) * 1000 }')

    local actual_exit=1
    if grep -q "^EXIT_CODE=" "$log_file" 2>/dev/null; then
        local ec_line
        ec_line=$(grep "^EXIT_CODE=" "$log_file" | tail -1)
        actual_exit="${ec_line#EXIT_CODE=}"
    fi

    rm -f "${log_file}.done"
    # Reap the background subshell to avoid zombie; ignore errors from
    # already-dead pids.
    wait "$run_pid" 2>/dev/null || true

    if grep -q "^WATCHDOG_TIMEOUT=1" "$log_file" 2>/dev/null; then
        RUN_STATUS="TIMEOUT"
    elif [[ "$actual_exit" == "0" ]]; then
        RUN_STATUS="PASS"
    else
        RUN_STATUS="FAIL"
    fi
}

# ── Markdown rendering ──
# Each row is "<size_key>|<env_name>|<env_value>|<elapsed_ms>|<status>".
# Tables are aggregated per size_key.
typeset -a SWEEP_ROWS
SWEEP_ROWS=()

record_row() {
    local size_key="$1"
    local env_name="$2"
    local env_value="$3"
    local elapsed_ms="$4"
    local run_status="$5"
    SWEEP_ROWS+=("${size_key}|${env_name}|${env_value}|${elapsed_ms}|${run_status}")
}

# Compute Δ% vs baseline per size_key. Baseline row has env_name "baseline".
delta_pct() {
    local baseline_ms="$1"
    local current_ms="$2"
    if [[ -z "$baseline_ms" || "$baseline_ms" -eq 0 ]]; then
        echo "-"
        return
    fi
    # Use awk for floating-point arithmetic to avoid zsh integer truncation.
    awk -v b="$baseline_ms" -v c="$current_ms" \
        'BEGIN { printf "%+.1f%%", (c - b) * 100.0 / b }'
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

render_markdown() {
    local outfile="$1"
    local mode="$2"  # "new" or "append"

    if [[ "$mode" == "new" ]]; then
        {
            echo "# GNFS Performance Sweep — $(date '+%Y-%m-%d %H:%M:%S')"
            echo
            echo "## Metadata"
            echo
            echo "| Key | Value |"
            echo "|-----|-------|"
            echo "| Host | $(hostname) |"
            echo "| Date | $(date '+%Y-%m-%d %H:%M:%S %Z') |"
            echo "| Build type | $BUILD_TYPE |"
            echo "| Git HEAD | $(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown) |"
            echo "| Branch | $(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown) |"
            echo "| CPU count | $NCPU |"
            echo "| Sizes | ${BIT_SIZES[*]:-(none)} bits / ${DIGIT_SIZES[*]:-(none)} digits |"
            echo "| ENV matrix | ${#SWEEP_MATRIX[@]} ENVs |"
            echo "| Sweep mode | single-variable vs baseline |"
            echo
        } > "$outfile"
    else
        {
            echo
            echo "---"
            echo
            echo "## Re-run $(date '+%Y-%m-%d %H:%M:%S')"
            echo
        } >> "$outfile"
    fi

    # Collect unique size keys preserving insertion order.
    typeset -a sizes_seen
    sizes_seen=()
    for row in "${SWEEP_ROWS[@]}"; do
        local sk="${row%%|*}"
        local found=0
        for s in "${sizes_seen[@]}"; do
            [[ "$s" == "$sk" ]] && { found=1; break; }
        done
        (( found )) || sizes_seen+=("$sk")
    done

    # Per-size tables.
    for sk in "${sizes_seen[@]}"; do
        local label=""
        case "$sk" in
            bit_*)   label="${sk#bit_}-bit (N=${TEST_N[$sk]})" ;;
            digit_*) label="${sk#digit_}-digit" ;;
            *)       label="$sk" ;;
        esac

        {
            echo "## Results: ${label}"
            echo
            echo "| ENV | Value | Wall time | Status | Δ vs baseline |"
            echo "|-----|-------|-----------|--------|---------------|"
        } >> "$outfile"

        # Baseline row first.
        local baseline_ms=0
        for row in "${SWEEP_ROWS[@]}"; do
            local rk="${row%%|*}"
            [[ "$rk" != "$sk" ]] && continue
            local rest="${row#*|}"
            local env_name="${rest%%|*}"; rest="${rest#*|}"
            local env_value="${rest%%|*}"; rest="${rest#*|}"
            local elapsed_ms="${rest%%|*}"; rest="${rest#*|}"
            local run_status="${rest%%|*}"
            if [[ "$env_name" == "baseline" ]]; then
                baseline_ms="$elapsed_ms"
                echo "| baseline | (none) | $(fmt_ms "$elapsed_ms") | $run_status | — |" >> "$outfile"
                break
            fi
        done

        # Non-baseline rows.
        for row in "${SWEEP_ROWS[@]}"; do
            local rk="${row%%|*}"
            [[ "$rk" != "$sk" ]] && continue
            local rest="${row#*|}"
            local env_name="${rest%%|*}"; rest="${rest#*|}"
            local env_value="${rest%%|*}"; rest="${rest#*|}"
            local elapsed_ms="${rest%%|*}"; rest="${rest#*|}"
            local run_status="${rest%%|*}"
            [[ "$env_name" == "baseline" ]] && continue
            local delta="—"
            if [[ "$run_status" == "PASS" ]]; then
                delta=$(delta_pct "$baseline_ms" "$elapsed_ms")
            fi
            echo "| \`$env_name\` | $env_value | $(fmt_ms "$elapsed_ms") | $run_status | $delta |" >> "$outfile"
        done

        echo >> "$outfile"
    done

    # Best ENV value per size — pick lowest wall time among PASS rows.
    {
        echo "## Best ENV Value per N Size"
        echo
        echo "Best = single-variable change with shortest wall time that still PASSes."
        echo "Baseline included when nothing beats it."
        echo
        echo "| N size | Best ENV | Best Value | Wall time | Δ vs baseline |"
        echo "|--------|----------|------------|-----------|---------------|"
    } >> "$outfile"

    for sk in "${sizes_seen[@]}"; do
        local label=""
        case "$sk" in
            bit_*)   label="${sk#bit_}-bit" ;;
            digit_*) label="${sk#digit_}-digit" ;;
            *)       label="$sk" ;;
        esac

        local best_env="baseline" best_value="(none)" best_ms=-1 base_ms=0
        for row in "${SWEEP_ROWS[@]}"; do
            local rk="${row%%|*}"
            [[ "$rk" != "$sk" ]] && continue
            local rest="${row#*|}"
            local env_name="${rest%%|*}"; rest="${rest#*|}"
            local env_value="${rest%%|*}"; rest="${rest#*|}"
            local elapsed_ms="${rest%%|*}"; rest="${rest#*|}"
            local run_status="${rest%%|*}"
            [[ "$run_status" != "PASS" ]] && continue
            if [[ "$env_name" == "baseline" ]]; then
                base_ms="$elapsed_ms"
            fi
            if (( best_ms < 0 || elapsed_ms < best_ms )); then
                best_ms="$elapsed_ms"
                best_env="$env_name"
                best_value="$env_value"
            fi
        done

        local delta="—"
        if (( best_ms >= 0 )) && [[ "$best_env" != "baseline" ]]; then
            delta=$(delta_pct "$base_ms" "$best_ms")
        fi
        if (( best_ms < 0 )); then
            echo "| $label | — | — | (no PASS) | — |" >> "$outfile"
        else
            local env_md="\`$best_env\`"
            [[ "$best_env" == "baseline" ]] && env_md="baseline"
            echo "| $label | $env_md | $best_value | $(fmt_ms "$best_ms") | $delta |" >> "$outfile"
        fi
    done

    echo >> "$outfile"
    log_success "Report written: $outfile"
}

# ── Main sweep loop ──
run_sweep_for_size() {
    local size_key="$1"
    local timeout_to_use="$2"

    resolve_command "$size_key" "$timeout_to_use" || return 1
    local cmd="$RESOLVED_CMD"
    local timeout="$RESOLVED_TIMEOUT"

    log_step "Size: $size_key ($RESOLVED_N_LABEL, per-run timeout ${timeout}s)"

    local tmpdir="${TMPDIR:-/tmp}/gnfs_sweep_$$_$size_key"
    mkdir -p "$tmpdir"

    # Run baseline first (no ENV set).
    log_info "  [baseline] $cmd"
    if (( DRY_RUN )); then
        echo "DRY: env (clean) $cmd  # timeout ${timeout}s"
        record_row "$size_key" "baseline" "(none)" "0" "DRY"
    else
        run_with_timeout "$cmd" "" "$timeout" "$tmpdir/baseline.log"
        log_info "    ${RUN_STATUS} in $(fmt_ms "$RUN_ELAPSED_MS")"
        record_row "$size_key" "baseline" "(none)" "$RUN_ELAPSED_MS" "$RUN_STATUS"
    fi

    # Run each ENV in matrix.
    for entry in "${SWEEP_MATRIX[@]}"; do
        local env_name="${entry%%:*}"
        local vals_str="${entry#*:}"
        local -a vals
        vals=(${(s:,:)vals_str})

        for val in "${vals[@]}"; do
            log_info "  [$env_name=$val] $cmd"
            if (( DRY_RUN )); then
                echo "DRY: env ${env_name}=${val} $cmd  # timeout ${timeout}s"
                record_row "$size_key" "$env_name" "$val" "0" "DRY"
                continue
            fi
            local safe_val="${val//[^a-zA-Z0-9_-]/_}"
            local log_path="$tmpdir/${env_name}_${safe_val}.log"
            run_with_timeout "$cmd" "${env_name}=${val}" "$timeout" "$log_path"
            log_info "    ${RUN_STATUS} in $(fmt_ms "$RUN_ELAPSED_MS")"
            record_row "$size_key" "$env_name" "$val" "$RUN_ELAPSED_MS" "$RUN_STATUS"
        done
    done
}

main() {
    filter_matrix
    if (( ${#SWEEP_MATRIX} == 0 )); then
        log_error "No ENVs in sweep matrix after filtering. Check --env-set names."
        exit 2
    fi

    ensure_build

    mkdir -p "$RESULTS_DIR"
    if [[ -z "$OUTPUT_FILE" ]]; then
        OUTPUT_FILE="${RESULTS_DIR}/sweep_$(date +%Y%m%d-%H%M%S).md"
    fi

    log_step "Sweep configuration"
    log_info "  Bit sizes:    ${BIT_SIZES[*]:-(none)}"
    log_info "  Digit sizes:  ${DIGIT_SIZES[*]:-(none)}"
    log_info "  ENV matrix:   ${#SWEEP_MATRIX[@]} ENVs"
    for entry in "${SWEEP_MATRIX[@]}"; do
        log_info "    - ${entry%%:*} ∈ {${entry#*:}}"
    done
    log_info "  Output:       $OUTPUT_FILE"
    log_info "  Dry run:      $DRY_RUN"

    for b in "${BIT_SIZES[@]}"; do
        run_sweep_for_size "bit_$b" "$TIMEOUT_SEC"
    done
    for d in "${DIGIT_SIZES[@]}"; do
        run_sweep_for_size "digit_$d" "$TIMEOUT_SEC"
    done

    local mode="new"
    (( APPEND_MODE )) && [[ -f "$OUTPUT_FILE" ]] && mode="append"
    render_markdown "$OUTPUT_FILE" "$mode"

    if (( DRY_RUN )); then
        log_success "Dry run complete (no commands executed)"
    else
        log_success "Sweep complete: ${#SWEEP_ROWS[@]} runs recorded"
    fi
}

main "$@"
