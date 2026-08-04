#!/bin/zsh

set -euo pipefail

if [[ $# -lt 5 || "$4" != "--" ]]; then
    print -u2 "usage: run-with-wall-timeout.sh <seconds> <log> <marker> -- <command> [args...]"
    exit 2
fi

timeout_seconds="$1"
log_path="$2"
timeout_marker="$3"
shift 4
kill_grace_seconds="${GNFS_WORKBENCH_TIMEOUT_KILL_GRACE_SECONDS:-10}"

for value in "$timeout_seconds" "$kill_grace_seconds"; do
    if [[ "$value" != <-> || "$value" -lt 1 ]]; then
        print -u2 "timeout and kill grace must be positive integers"
        exit 2
    fi
done

mkdir -p "${log_path:h}" "${timeout_marker:h}"
rm -f -- "$timeout_marker"
command_pid=""
watchdog_pid=""

terminate_group() {
    local signal_name="$1"
    [[ -n "$command_pid" ]] || return 0
    /bin/kill -"$signal_name" -- -"$command_pid" 2>/dev/null || true
}

abort_run() {
    local exit_code="$1"
    trap - INT TERM
    if [[ -n "$watchdog_pid" ]]; then
        kill -TERM "$watchdog_pid" 2>/dev/null || true
        wait "$watchdog_pid" 2>/dev/null || true
        watchdog_pid=""
    fi
    terminate_group TERM
    sleep "$kill_grace_seconds"
    terminate_group KILL
    if [[ -n "$command_pid" ]]; then
        wait "$command_pid" 2>/dev/null || true
        command_pid=""
    fi
    exit "$exit_code"
}
trap 'abort_run 130' INT
trap 'abort_run 143' TERM

/usr/bin/python3 -c \
    'import os, sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])' \
    "$@" > "$log_path" 2>&1 &
command_pid=$!

(
    elapsed=0
    while (( elapsed < timeout_seconds )); do
        remaining=$((timeout_seconds - elapsed))
        interval=$((remaining < 30 ? remaining : 30))
        sleep "$interval"
        /bin/kill -0 "$command_pid" 2>/dev/null || exit 0
        elapsed=$((elapsed + interval))
        print "Workbench tests are still running (${elapsed}s elapsed)."
    done
    /usr/bin/touch "$timeout_marker"
    print -u2 "Workbench test wall timeout reached after ${timeout_seconds}s."
    terminate_group TERM
    sleep "$kill_grace_seconds"
    terminate_group KILL
) &
watchdog_pid=$!

set +e
wait "$command_pid"
command_exit_code=$?
set -e
if [[ -f "$timeout_marker" ]]; then
    # The leader may exit on TERM while descendants in the same process group
    # keep running. Let the watchdog complete its grace period and group KILL.
    wait "$watchdog_pid" 2>/dev/null || true
else
    kill -TERM "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
fi
command_pid=""
watchdog_pid=""
trap - INT TERM

/bin/cat "$log_path"
if [[ -f "$timeout_marker" ]]; then
    exit 124
fi
exit "$command_exit_code"
