#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
scratch_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-publish-tests.XXXXXX")

cleanup() {
    rm -rf -- "$scratch_root"
}
trap cleanup EXIT INT TERM

make_pair() {
    local directory="$1"
    local payload="$2"
    mkdir -p "$directory"
    /usr/bin/printf '%s\n' "$payload" > "$directory/artifact.zip"
    (
        cd "$directory"
        /usr/bin/shasum -a 256 artifact.zip > artifact.zip.sha256
    )
}

verify_pair() {
    local directory="$1"
    local expected="$2"
    (
        cd "$directory"
        /usr/bin/shasum -a 256 -c artifact.zip.sha256 >/dev/null
    )
    [[ "$(<"$directory/artifact.zip")" == "$expected" ]]
}

verify_no_pair() {
    local directory="$1"
    [[ ! -e "$directory/artifact.zip" ]]
    [[ ! -e "$directory/artifact.zip.sha256" ]]
}

verify_no_transaction_state() {
    local directory="$1"
    [[ ! -e "$directory/.gnfs-workbench-publish.lock" ]]
    [[ -z "$(find "$directory" -maxdepth 1 \
        -name '.gnfs-workbench-publish.*' -print -quit)" ]]
}

expect_fault() {
    local mode="$1"
    local stage="$2"
    local source="$3"
    local output="$4"

    if GNFS_WORKBENCH_PUBLISH_TEST_FAULT_MODE="$mode" \
        GNFS_WORKBENCH_PUBLISH_TEST_FAULT_STAGE="$stage" \
        "$script_dir/publish-package.sh" \
            "$source/artifact.zip" "$source/artifact.zip.sha256" \
            "$output" >/dev/null 2>&1; then
        print -u2 "fault did not fail publication: $mode $stage"
        return 1
    fi
}

wait_for_marker() {
    local marker="$1"
    local attempt
    for attempt in {1..500}; do
        [[ -f "$marker" ]] && return 0
        sleep 0.01
    done
    print -u2 "timed out waiting for publish fault marker"
    return 1
}

first="$scratch_root/first"
second="$scratch_root/second"
third="$scratch_root/third"
make_pair "$first" "first"
make_pair "$second" "second"
make_pair "$third" "third"

fault_stages=(
    after-backup-archive
    after-backup-checksum
    after-new-archive
    after-new-checksum
)
fault_modes=(term fail)
for mode in "${fault_modes[@]}"; do
    for stage in "${fault_stages[@]}"; do
        replacement_output="$scratch_root/replacement-$mode-$stage"
        "$script_dir/publish-package.sh" \
            "$first/artifact.zip" "$first/artifact.zip.sha256" \
            "$replacement_output" >/dev/null
        expect_fault "$mode" "$stage" "$second" "$replacement_output"
        verify_pair "$replacement_output" "first"
        verify_no_transaction_state "$replacement_output"

        initial_output="$scratch_root/initial-$mode-$stage"
        expect_fault "$mode" "$stage" "$first" "$initial_output"
        verify_no_pair "$initial_output"
        verify_no_transaction_state "$initial_output"
    done
done

output="$scratch_root/concurrent-output"
"$script_dir/publish-package.sh" \
    "$first/artifact.zip" "$first/artifact.zip.sha256" "$output" >/dev/null
concurrency_marker="$scratch_root/concurrency.marker"
GNFS_WORKBENCH_PUBLISH_TEST_FAULT_MODE=pause \
GNFS_WORKBENCH_PUBLISH_TEST_FAULT_STAGE=after-new-archive \
GNFS_WORKBENCH_PUBLISH_TEST_MARKER="$concurrency_marker" \
GNFS_WORKBENCH_PUBLISH_TEST_PAUSE_SECONDS=1 \
    "$script_dir/publish-package.sh" \
        "$second/artifact.zip" "$second/artifact.zip.sha256" "$output" >/dev/null &
first_publisher=$!
wait_for_marker "$concurrency_marker"
if "$script_dir/publish-package.sh" \
    "$third/artifact.zip" "$third/artifact.zip.sha256" "$output" >/dev/null 2>&1; then
    print -u2 "concurrent publisher unexpectedly acquired the output lock"
    exit 1
fi
wait "$first_publisher"
verify_pair "$output" "second"
verify_no_transaction_state "$output"

source_link="$scratch_root/source-link.zip"
ln -s "$first/artifact.zip" "$source_link"
if "$script_dir/publish-package.sh" \
    "$source_link" "$first/artifact.zip.sha256" \
    "$scratch_root/source-link-output" >/dev/null 2>&1; then
    print -u2 "publisher unexpectedly accepted a symbolic-link source"
    exit 1
fi

misdirected="$scratch_root/misdirected"
mkdir -p "$misdirected"
/usr/bin/printf '%s\n' "unverified archive" > "$misdirected/artifact.zip"
/usr/bin/printf '%s\n' "different file" > "$misdirected/different.zip"
misdirected_hash=$(/usr/bin/shasum -a 256 "$misdirected/different.zip" | cut -d ' ' -f 1)
/usr/bin/printf '%s  %s\n' \
    "$misdirected_hash" "$misdirected/different.zip" \
    > "$misdirected/artifact.zip.sha256"
if "$script_dir/publish-package.sh" \
    "$misdirected/artifact.zip" "$misdirected/artifact.zip.sha256" \
    "$scratch_root/misdirected-output" >/dev/null 2>&1; then
    print -u2 "publisher accepted a checksum manifest for a different file"
    exit 1
fi
verify_no_pair "$scratch_root/misdirected-output"

linked_output_target="$scratch_root/linked-output-target"
linked_output="$scratch_root/linked-output"
mkdir -p "$linked_output_target"
ln -s "$linked_output_target" "$linked_output"
for linked_output_path in "$linked_output" "$linked_output/" "$linked_output/."; do
    if "$script_dir/publish-package.sh" \
        "$first/artifact.zip" "$first/artifact.zip.sha256" \
        "$linked_output_path" >/dev/null 2>&1; then
        print -u2 "publisher unexpectedly accepted a symbolic-link output"
        exit 1
    fi
done
verify_no_pair "$linked_output_target"

license_destination="$scratch_root/licenses"
"$script_dir/embed-license-resources.sh" "$license_destination"
[[ $(find "$license_destination" -mindepth 1 -maxdepth 1 -type f | wc -l | tr -d ' ') -eq 6 ]]
license_link_target="$scratch_root/license-link-target"
license_link="$scratch_root/license-link"
mkdir -p "$license_link_target"
ln -s "$license_link_target" "$license_link"
for license_link_path in "$license_link" "$license_link/" "$license_link/."; do
    if "$script_dir/embed-license-resources.sh" "$license_link_path" >/dev/null 2>&1; then
        print -u2 "license embedder unexpectedly accepted a symbolic-link destination"
        exit 1
    fi
done
[[ -z "$(find "$license_link_target" -mindepth 1 -print -quit)" ]]

print "Workbench package publication and license embedding tests passed"
