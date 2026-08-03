#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
scratch_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-timeout-tests.XXXXXX")

cleanup() {
    rm -rf -- "$scratch_root"
}
trap cleanup EXIT INT TERM

write_fixture() {
    local fixture_root="$1"
    mkdir -p "$fixture_root"
    /usr/bin/printf '%s\n' \
        '#!/bin/zsh' \
        'trap "" TERM' \
        'print -r -- "$$" > "$GNFS_TIMEOUT_TEST_ROOT/grandchild.pid"' \
        'while true; do /bin/sleep 1; done' > "$fixture_root/grandchild.zsh"
    /usr/bin/printf '%s\n' \
        '#!/bin/zsh' \
        'trap "" TERM' \
        '"$GNFS_TIMEOUT_TEST_ROOT/grandchild.zsh" &' \
        'wait' > "$fixture_root/child.zsh"
    /usr/bin/printf '%s\n' \
        '#!/bin/zsh' \
        'trap "" TERM' \
        'print -r -- "$$" > "$GNFS_TIMEOUT_TEST_ROOT/root.pid"' \
        '"$GNFS_TIMEOUT_TEST_ROOT/child.zsh" &' \
        'print -r -- "$!" > "$GNFS_TIMEOUT_TEST_ROOT/child.pid"' \
        'wait' > "$fixture_root/root.zsh"
    chmod 0755 \
        "$fixture_root/grandchild.zsh" \
        "$fixture_root/child.zsh" \
        "$fixture_root/root.zsh"
}

wait_for_fixture() {
    local fixture_root="$1"
    for _ in {1..250}; do
        if [[ -s "$fixture_root/root.pid" && -s "$fixture_root/child.pid" && \
              -s "$fixture_root/grandchild.pid" ]]; then
            return 0
        fi
        /bin/sleep 0.02
    done
    return 1
}

assert_fixture_stopped() {
    local fixture_root="$1"
    local pid_file process_id stopped
    for pid_file in root.pid child.pid grandchild.pid; do
        if [[ ! -s "$fixture_root/$pid_file" ]]; then
            print -u2 "timeout fixture did not record $pid_file"
            return 1
        fi
        process_id=$(<"$fixture_root/$pid_file")
        stopped=false
        for _ in {1..100}; do
            if ! /bin/kill -0 "$process_id" 2>/dev/null; then
                stopped=true
                break
            fi
            /bin/sleep 0.02
        done
        if [[ "$stopped" != true ]]; then
            print -u2 "timeout helper left process $process_id from $pid_file running"
            return 1
        fi
    done
}

timeout_root="$scratch_root/timeout"
write_fixture "$timeout_root"

set +e
GNFS_TIMEOUT_TEST_ROOT="$timeout_root" \
GNFS_WORKBENCH_TIMEOUT_KILL_GRACE_SECONDS=1 \
    "$script_dir/run-with-wall-timeout.sh" \
        3 "$timeout_root/probe.log" "$timeout_root/timed-out" -- \
        "$timeout_root/root.zsh"
timeout_exit_code=$?
set -e
if [[ "$timeout_exit_code" -ne 124 ]]; then
    print -u2 "wall-timeout helper returned $timeout_exit_code instead of 124"
    exit 1
fi
assert_fixture_stopped "$timeout_root"

external_root="$scratch_root/external"
write_fixture "$external_root"
GNFS_TIMEOUT_TEST_ROOT="$external_root" \
GNFS_WORKBENCH_TIMEOUT_KILL_GRACE_SECONDS=1 \
    "$script_dir/run-with-wall-timeout.sh" \
        120 "$external_root/probe.log" "$external_root/timed-out" -- \
        "$external_root/root.zsh" > "$external_root/helper.log" 2>&1 &
helper_pid=$!
if ! wait_for_fixture "$external_root"; then
    print -u2 "external-cancel fixture did not start"
    /bin/kill -TERM "$helper_pid" 2>/dev/null || true
    wait "$helper_pid" 2>/dev/null || true
    exit 1
fi
/bin/kill -TERM "$helper_pid"
set +e
wait "$helper_pid"
external_exit_code=$?
set -e
if [[ "$external_exit_code" -ne 143 ]]; then
    print -u2 "externally cancelled helper returned $external_exit_code instead of 143"
    exit 1
fi
if [[ -e "$external_root/timed-out" ]]; then
    print -u2 "external cancellation incorrectly wrote the timeout marker"
    exit 1
fi
assert_fixture_stopped "$external_root"

print "Workbench wall-timeout process-group and external-cancel tests passed"
