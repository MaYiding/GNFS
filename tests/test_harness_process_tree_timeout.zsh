#!/usr/bin/env zsh

set -eu
unsetopt BG_NICE

if [[ "${1:-}" == "--wrapper-signal-contract" ]]; then
    if (( $# != 7 && $# != 9 )); then
        exit 64
    fi
    supervisor="${2:A}"
    fake_child="${3:A}"
    ready_file="${4:A}"
    survived_file="${5:A}"
    wrapper_mode="$6"
    trap_marker="${7:A}"
    PROJECT_ROOT="${0:A:h:h}"
    BUILD_DIR="${supervisor:h}"
    BUILD_TYPE="Debug"
    QUIET=1
    GNFS_TEST_PROCESS_SUPERVISOR="$supervisor"
    source "${PROJECT_ROOT}/scripts/lib/process_tree_timeout.zsh"

    # The scoped wrapper traps must restore and re-deliver to an existing
    # caller trap only after the supervisor has completed containment cleanup.
    trap 'print -r -- HUP >"$trap_marker"' HUP
    trap 'print -r -- INT >"$trap_marker"' INT
    trap 'print -r -- TERM >"$trap_marker"' TERM
    contract_status=0
    if [[ "$wrapper_mode" == "combined" && $# == 7 ]]; then
        run_with_timeout 10 "$fake_child" --timeout-tree \
            "$survived_file" 1000 "$ready_file" || contract_status=$?
        print -r -- "$RUN_OUTPUT"
    elif [[ "$wrapper_mode" == "dual" && $# == 9 ]]; then
        run_with_timeout_to_files "${8:A}" "${9:A}" 10 \
            "$fake_child" --timeout-tree \
            "$survived_file" 1000 "$ready_file" || contract_status=$?
    else
        exit 64
    fi
    exit "$contract_status"
fi

if (( $# != 3 )); then
    print -u2 -r -- "usage: $0 SUPERVISOR FAKE_CHILD ZSH_EXECUTABLE"
    exit 64
fi

supervisor="${1:A}"
fake_child="${2:A}"
zsh_executable="${3:A}"
if [[ ! -x "$supervisor" || ! -x "$fake_child" || ! -x "$zsh_executable" ]]; then
    print -u2 -r -- "supervisor or fake child is not executable"
    exit 1
fi

PROJECT_ROOT="${0:A:h:h}"
BUILD_DIR="${supervisor:h}"
BUILD_TYPE="Debug"
QUIET=1
GNFS_TEST_PROCESS_SUPERVISOR="$supervisor"
source "${PROJECT_ROOT}/scripts/lib/process_tree_timeout.zsh"

test_directory=$(mktemp -d "${TMPDIR:-/tmp}/gnfs-harness-timeout.XXXXXX")
trap 'rm -r -- "$test_directory"' EXIT
typeset -a cancellation_survived_files
cancellation_survived_files=()

contract_status=0
run_with_timeout 3 "$fake_child" --nonzero || contract_status=$?
if (( contract_status != 23 )) ||
   [[ "$RUN_OUTPUT" != $'stdout-before-nonzero\nstderr-before-nonzero' ]]; then
    print -u2 -r -- \
        "combined wrapper did not preserve nonzero output/exit: status=$contract_status"
    exit 1
fi

stdout_file="${test_directory}/dual.stdout"
stderr_file="${test_directory}/dual.stderr"
contract_status=0
run_with_timeout_to_files "$stdout_file" "$stderr_file" 3 \
    "$fake_child" --nonzero || contract_status=$?
if (( contract_status != 23 )) ||
   [[ "$(<"$stdout_file")" != "stdout-before-nonzero" ]] ||
   [[ "$(<"$stderr_file")" != "stderr-before-nonzero" ]]; then
    print -u2 -r -- \
        "dual wrapper did not preserve stream bytes/exit: status=$contract_status"
    exit 1
fi

ready_file="${test_directory}/timeout.ready"
survived_file="${test_directory}/timeout.survived"
contract_status=0
run_with_timeout 2 "$fake_child" --timeout-tree \
    "$survived_file" 2500 "$ready_file" || contract_status=$?
if (( contract_status != 124 )) || [[ "$RUN_OUTPUT" != "descendant-ready" ]] ||
   [[ ! -f "$ready_file" || "$(<"$ready_file")" != "tree-ready" ]] ||
   [[ ! -f "${ready_file}.descendant" ||
      "$(<"${ready_file}.descendant")" != "descendant-ready" ]]; then
    print -u2 -r -- \
        "tree timeout contract failed: status=$contract_status output=$RUN_OUTPUT"
    exit 1
fi
sleep 3
if [[ -e "$survived_file" ]]; then
    print -u2 -r -- "tree timeout left a descendant alive"
    exit 1
fi

for signal_name in HUP INT TERM; do
    case "$signal_name" in
        HUP) expected_status=129 ;;
        INT) expected_status=130 ;;
        TERM) expected_status=143 ;;
    esac
    ready_file="${test_directory}/signal-${signal_name}.ready"
    survived_file="${test_directory}/signal-${signal_name}.survived"
    signal_stdout="${test_directory}/signal-${signal_name}.stdout"
    signal_stderr="${test_directory}/signal-${signal_name}.stderr"
    trap_marker="${test_directory}/signal-${signal_name}.trap"
    "$zsh_executable" "${0:A}" --wrapper-signal-contract \
        "$supervisor" "$fake_child" "$ready_file" "$survived_file" combined "$trap_marker" \
        >"$signal_stdout" 2>"$signal_stderr" &
    wrapper_pid=$!

    ready=0
    for (( poll = 0; poll < 200; poll += 1 )); do
        if [[ -f "$ready_file" && "$(<"$ready_file")" == "tree-ready" ]]; then
            ready=1
            break
        fi
        sleep 0.01
    done
    if (( !ready )); then
        print -u2 -r -- "signal $signal_name never reached descendant-ready"
        wait "$wrapper_pid" 2>/dev/null || true
        exit 1
    fi

    kill -"$signal_name" "$wrapper_pid"
    contract_status=0
    wait "$wrapper_pid" || contract_status=$?
    if (( contract_status != expected_status )); then
        print -u2 -r -- \
            "signal $signal_name exit changed: expected=$expected_status actual=$contract_status"
        exit 1
    fi
    cancellation_survived_files+=("$survived_file")
    if [[ "$(<"$ready_file")" != "tree-ready" ||
          "$(<"${ready_file}.descendant")" != "descendant-ready" ||
          "$(<"$signal_stdout")" != "descendant-ready" ]]; then
        print -u2 -r -- "signal $signal_name lost captured child output"
        exit 1
    fi
    if [[ "$(<"$trap_marker")" != "$signal_name" || -s "$signal_stderr" ]]; then
        print -u2 -r -- "signal $signal_name trap restoration/control stderr changed"
        exit 1
    fi
done

ready_file="${test_directory}/signal-dual-TERM.ready"
survived_file="${test_directory}/signal-dual-TERM.survived"
signal_stdout="${test_directory}/signal-dual-TERM.stdout"
signal_stderr="${test_directory}/signal-dual-TERM.stderr"
child_stdout="${test_directory}/signal-dual-child.stdout"
child_stderr="${test_directory}/signal-dual-child.stderr"
trap_marker="${test_directory}/signal-dual-TERM.trap"
"$zsh_executable" "${0:A}" --wrapper-signal-contract \
    "$supervisor" "$fake_child" "$ready_file" "$survived_file" dual \
    "$trap_marker" "$child_stdout" "$child_stderr" \
    >"$signal_stdout" 2>"$signal_stderr" &
wrapper_pid=$!
ready=0
for (( poll = 0; poll < 200; poll += 1 )); do
    if [[ -f "$ready_file" && "$(<"$ready_file")" == "tree-ready" ]]; then
        ready=1
        break
    fi
    sleep 0.01
done
if (( !ready )); then
    print -u2 -r -- "dual wrapper TERM never reached descendant-ready"
    wait "$wrapper_pid" 2>/dev/null || true
    exit 1
fi
kill -TERM "$wrapper_pid"
contract_status=0
wait "$wrapper_pid" || contract_status=$?
cancellation_survived_files+=("$survived_file")
if (( contract_status != 143 )) ||
   [[ "$(<"$ready_file")" != "tree-ready" ]] ||
   [[ "$(<"${ready_file}.descendant")" != "descendant-ready" ]] ||
   [[ "$(<"$child_stdout")" != "descendant-ready" ]] || [[ -s "$child_stderr" ]] ||
   [[ "$(<"$trap_marker")" != "TERM" ]] || [[ -s "$signal_stdout" ]] ||
   [[ -s "$signal_stderr" ]]; then
    print -u2 -r -- "dual wrapper TERM cancellation contract failed"
    exit 1
fi

failure_supervisor="${test_directory}/cleanup-failure-supervisor.zsh"
{
    print -r -- '#!/usr/bin/env zsh'
    print -r -- 'setopt trapsasync'
    print -r -- 'zmodload zsh/zselect || exit 126'
    print -r -- 'trap '\''exit 125'\'' HUP INT TERM'
    print -r -- ': > "${@[-1]}"'
    print -r -- 'zselect -t 500'
    print -r -- 'exit 126'
} >"$failure_supervisor"
chmod +x "$failure_supervisor"
failure_ready="${test_directory}/cleanup-failure.ready"
failure_survived="${test_directory}/cleanup-failure.survived"
failure_trap="${test_directory}/cleanup-failure.trap"
failure_stdout="${test_directory}/cleanup-failure.stdout"
failure_stderr="${test_directory}/cleanup-failure.stderr"
"$zsh_executable" "${0:A}" --wrapper-signal-contract \
    "$failure_supervisor" "$fake_child" "$failure_ready" "$failure_survived" \
    combined "$failure_trap" >"$failure_stdout" 2>"$failure_stderr" &
wrapper_pid=$!
ready=0
for (( poll = 0; poll < 200; poll += 1 )); do
    if [[ -f "$failure_ready" ]]; then
        ready=1
        break
    fi
    sleep 0.01
done
if (( !ready )); then
    print -u2 -r -- "cleanup-failure supervisor never became ready"
    wait "$wrapper_pid" 2>/dev/null || true
    exit 1
fi
kill -TERM "$wrapper_pid"
contract_status=0
wait "$wrapper_pid" || contract_status=$?
if (( contract_status != 125 )) || [[ -e "$failure_trap" ]] ||
   [[ -s "$failure_stderr" ]]; then
    print -u2 -r -- "supervisor cleanup failure was masked by wrapper signal status"
    exit 1
fi

sleep 2.2
for survived_file in "${cancellation_survived_files[@]}"; do
    if [[ -e "$survived_file" ]]; then
        print -u2 -r -- "wrapper cancellation left a descendant alive: $survived_file"
        exit 1
    fi
done

print -r -- "[PASS] Harness process-tree timeout and cancellation contracts"
