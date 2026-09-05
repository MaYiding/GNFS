# Shared launch-domain timeout wrappers for scripts/test.sh and their contract
# test. The C++ supervisor owns the POSIX process group or Windows Job.

GNFS_TEST_PROCESS_OUTPUT_LIMIT_BYTES=16777216
GNFS_TEST_PROCESS_FORWARDED_SIGNAL_NAME=""
integer GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS=0
RUN_OUTPUT=""

gnfs_forward_process_supervisor_signal() {
    local signal_name="$1"
    local signal_exit_code="$2"
    if (( gnfs_forwarded_signal_status == 0 )); then
        gnfs_forwarded_signal_name="$signal_name"
        gnfs_forwarded_signal_status="$signal_exit_code"
    fi
    (( gnfs_forwarded_signal_generation += 1 ))
    if (( gnfs_active_supervisor_pid > 0 )); then
        kill -"$signal_name" "$gnfs_active_supervisor_pid" 2>/dev/null || true
    fi
}

gnfs_run_process_supervisor() {
    emulate -L zsh
    setopt localtraps trapsasync
    unsetopt bgnice
    local control_stdout_file="$1"
    shift
    local -a supervisor_command=("$@")
    if (( ${#supervisor_command[@]} == 0 )); then
        return 125
    fi

    local -i gnfs_active_supervisor_pid=0
    local -i gnfs_forwarded_signal_status=0
    local -i gnfs_forwarded_signal_generation=0
    local gnfs_forwarded_signal_name=""
    trap 'gnfs_forward_process_supervisor_signal HUP 129' HUP
    trap 'gnfs_forward_process_supervisor_signal INT 130' INT
    trap 'gnfs_forward_process_supervisor_signal TERM 143' TERM

    "${supervisor_command[@]}" >"$control_stdout_file" &
    gnfs_active_supervisor_pid=$!
    if (( gnfs_forwarded_signal_status != 0 )); then
        kill -"$gnfs_forwarded_signal_name" "$gnfs_active_supervisor_pid" 2>/dev/null || true
    fi

    local supervisor_exit_code=0
    local observed_supervisor_failure=0
    local wait_retried_after_signal=0
    while true; do
        local signal_generation_before_wait=$gnfs_forwarded_signal_generation
        if wait "$gnfs_active_supervisor_pid"; then
            supervisor_exit_code=0
        else
            supervisor_exit_code=$?
        fi
        (( supervisor_exit_code == 125 )) && observed_supervisor_failure=1
        if (( gnfs_forwarded_signal_generation == signal_generation_before_wait )); then
            break
        fi
        wait_retried_after_signal=1
    done
    if (( observed_supervisor_failure )); then
        supervisor_exit_code=125
    elif (( wait_retried_after_signal && supervisor_exit_code == 127 &&
            gnfs_forwarded_signal_status != 0 )); then
        supervisor_exit_code=$gnfs_forwarded_signal_status
    fi

    GNFS_TEST_PROCESS_FORWARDED_SIGNAL_NAME="$gnfs_forwarded_signal_name"
    GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS=$gnfs_forwarded_signal_status
    return "$supervisor_exit_code"
}

gnfs_rethrow_process_supervisor_signal() {
    if (( GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS == 0 )); then
        return 0
    fi
    local signal_name="$GNFS_TEST_PROCESS_FORWARDED_SIGNAL_NAME"
    local signal_exit_code="$GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS"
    local shell_pid="$$"
    if zmodload zsh/system 2>/dev/null && [[ -n "${sysparams[pid]:-}" ]]; then
        shell_pid="${sysparams[pid]}"
    fi
    kill -"$signal_name" "$shell_pid" 2>/dev/null || return 125
    return "$signal_exit_code"
}

gnfs_process_supervisor_control_file() {
    local temp_root="${TMPDIR:-/tmp}"
    [[ "$temp_root" == /* ]] || temp_root=/tmp
    mktemp "${temp_root%/}/gnfs-test-supervisor.XXXXXX"
}

gnfs_test_process_supervisor_path() {
    local configured="${GNFS_TEST_PROCESS_SUPERVISOR:-}"
    local -a candidates
    if [[ -n "$configured" ]]; then
        candidates=("$configured")
    else
        candidates=(
            "${BUILD_DIR}/gnfs_test_process_supervisor"
            "${BUILD_DIR}/gnfs_test_process_supervisor.exe"
            "${BUILD_DIR}/${BUILD_TYPE:-Debug}/gnfs_test_process_supervisor.exe"
        )
    fi

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            print -r -- "$candidate"
            return 0
        fi
    done
    return 1
}

gnfs_validate_process_timeout_request() {
    local timeout_seconds="$1"
    local executable="$2"
    [[ "$timeout_seconds" =~ '^[1-9][0-9]*$' ]] || return 1
    (( timeout_seconds <= 9223372036854 )) || return 1
    [[ "$executable" == /* && -x "$executable" ]] || return 1
}

gnfs_process_supervisor_heartbeat_seconds() {
    if (( ${QUIET:-0} )); then
        print -r -- 0
    else
        print -r -- 10
    fi
}

# run_with_timeout <timeout_secs> <absolute_command> [args...]
# Returns the command exit code, 124 for a process-tree timeout, and 125 for a
# supervisor failure. Captured stdout plus stderr is stored in RUN_OUTPUT with
# the same spawn-time 2>&1 ordering as the historical wrapper.
run_with_timeout() {
    local timeout_seconds="$1"
    shift
    local -a command=("$@")
    local supervisor
    if (( ${#command[@]} == 0 )) ||
       ! gnfs_validate_process_timeout_request "$timeout_seconds" "$command[1]" ||
       ! supervisor=$(gnfs_test_process_supervisor_path); then
        RUN_OUTPUT="gnfs test supervisor unavailable or invalid timeout request"
        return 125
    fi

    local control_stdout_file
    if ! control_stdout_file=$(gnfs_process_supervisor_control_file); then
        RUN_OUTPUT="gnfs test supervisor could not create its capture file"
        return 125
    fi
    local heartbeat_seconds
    heartbeat_seconds=$(gnfs_process_supervisor_heartbeat_seconds)
    local exit_code=0
    gnfs_run_process_supervisor "$control_stdout_file" "$supervisor" \
        --timeout-ms "$((timeout_seconds * 1000))" \
        --heartbeat-seconds "$heartbeat_seconds" \
        --output-limit-bytes "$GNFS_TEST_PROCESS_OUTPUT_LIMIT_BYTES" \
        --combined-output \
        -- "${command[@]}" || exit_code=$?
    RUN_OUTPUT=$(<"$control_stdout_file")
    rm -f -- "$control_stdout_file"
    if (( GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS != 0 && exit_code != 125 )); then
        gnfs_rethrow_process_supervisor_signal
        return "$GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS"
    fi
    return "$exit_code"
}

# run_with_timeout_to_files <stdout> <stderr> <timeout_secs> <absolute_command> [args...]
# The two child streams remain byte-distinct. Supervisor diagnostics and
# heartbeats continue to use the parent's stderr and never enter either file.
run_with_timeout_to_files() {
    local stdout_file="$1"
    local stderr_file="$2"
    local timeout_seconds="$3"
    shift 3
    local -a command=("$@")
    local supervisor
    if (( ${#command[@]} == 0 )) || [[ "$stdout_file" != /* || "$stderr_file" != /* ]] ||
       [[ "$stdout_file" == "$stderr_file" ]] ||
       ! gnfs_validate_process_timeout_request "$timeout_seconds" "$command[1]" ||
       ! supervisor=$(gnfs_test_process_supervisor_path); then
        return 125
    fi

    local control_stdout_file
    if ! control_stdout_file=$(gnfs_process_supervisor_control_file); then
        return 125
    fi
    local heartbeat_seconds
    heartbeat_seconds=$(gnfs_process_supervisor_heartbeat_seconds)
    local exit_code=0
    gnfs_run_process_supervisor "$control_stdout_file" "$supervisor" \
        --timeout-ms "$((timeout_seconds * 1000))" \
        --heartbeat-seconds "$heartbeat_seconds" \
        --output-limit-bytes "$GNFS_TEST_PROCESS_OUTPUT_LIMIT_BYTES" \
        --stdout-file "$stdout_file" \
        --stderr-file "$stderr_file" \
        -- "${command[@]}" || exit_code=$?
    local control_output_changed=0
    [[ -s "$control_stdout_file" ]] && control_output_changed=1
    rm -f -- "$control_stdout_file"
    (( control_output_changed )) && exit_code=125
    if (( GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS != 0 && exit_code != 125 )); then
        gnfs_rethrow_process_supervisor_signal
        return "$GNFS_TEST_PROCESS_FORWARDED_SIGNAL_STATUS"
    fi
    return "$exit_code"
}
