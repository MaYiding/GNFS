#!/bin/zsh

set -euo pipefail

if [[ $# -ne 3 ]]; then
    print -u2 "usage: publish-package.sh <archive> <checksum> <output-directory>"
    exit 2
fi

strip_directory_suffixes() {
    REPLY="$1"
    while [[ "$REPLY" != "/" ]]; do
        if [[ "$REPLY" == */ ]]; then
            REPLY="${REPLY%/}"
            continue
        fi
        if [[ "$REPLY" == */. ]]; then
            REPLY="${REPLY%/.}"
            [[ -n "$REPLY" ]] || REPLY="/"
            continue
        fi
        break
    done
}

verify_checksum_pair() {
    local archive_path="$1"
    local checksum_path="$2"
    local expected_name="$3"
    local line_count
    local expected_hash
    local actual_hash

    line_count=$(wc -l < "$checksum_path" | tr -d ' ')
    if [[ "$line_count" -ne 1 ]]; then
        print -u2 "checksum manifest must contain exactly one newline-terminated entry"
        return 1
    fi
    expected_hash=$(awk -v expected_name="$expected_name" '
        NR != 1 { exit 1 }
        {
            if (length($0) != 66 + length(expected_name)) exit 1
            hash = substr($0, 1, 64)
            if (hash ~ /[^0-9a-f]/) exit 1
            if (substr($0, 65, 2) != "  ") exit 1
            if (substr($0, 67) != expected_name) exit 1
            print hash
        }
        END { if (NR != 1) exit 1 }
    ' "$checksum_path") || {
        print -u2 "checksum manifest must be '<sha256>  $expected_name'"
        return 1
    }
    actual_hash=$(/usr/bin/shasum -a 256 "$archive_path" | /usr/bin/cut -d ' ' -f 1)
    if [[ "$actual_hash" != "$expected_hash" ]]; then
        print -u2 "archive SHA-256 does not match its checksum manifest"
        return 1
    fi
}

source_archive_input="$1"
source_checksum_input="$2"
output_dir_input="$3"
strip_directory_suffixes "$source_archive_input"
source_archive_leaf="$REPLY"
strip_directory_suffixes "$source_checksum_input"
source_checksum_leaf="$REPLY"
strip_directory_suffixes "$output_dir_input"
output_dir_leaf="$REPLY"
if [[ -L "$source_archive_leaf" || -L "$source_checksum_leaf" ]]; then
    print -u2 "publish sources must not be symbolic links"
    exit 1
fi
if [[ -L "$output_dir_leaf" ]]; then
    print -u2 "publish output must not be a symbolic link"
    exit 1
fi
source_archive="${source_archive_input:A}"
source_checksum="${source_checksum_input:A}"
output_dir="${output_dir_input:A}"
archive_name="${source_archive:t}"
checksum_name="${source_checksum:t}"

if [[ "$checksum_name" != "$archive_name.sha256" ]]; then
    print -u2 "checksum filename must be <archive>.sha256"
    exit 2
fi
for source_file in "$source_archive" "$source_checksum"; do
    if [[ ! -f "$source_file" || -L "$source_file" ]]; then
        print -u2 "publish source must be a regular file: $source_file"
        exit 1
    fi
done

if [[ -e "$output_dir" && (! -d "$output_dir" || -L "$output_dir") ]]; then
    print -u2 "publish output must be a real directory: $output_dir"
    exit 1
fi
mkdir -p "$output_dir"

lock_dir="$output_dir/.gnfs-workbench-publish.lock"
if ! mkdir "$lock_dir" 2>/dev/null; then
    print -u2 "another Workbench package publication owns $lock_dir"
    exit 73
fi

transaction_dir=""
staged_dir=""
backup_dir=""
archive_path="$output_dir/$archive_name"
checksum_path="$output_dir/$checksum_name"
publishing=false
committed=false

fault_point() {
    local stage="$1"
    [[ "${GNFS_WORKBENCH_PUBLISH_TEST_FAULT_STAGE:-}" == "$stage" ]] || return 0
    if [[ -n "${GNFS_WORKBENCH_PUBLISH_TEST_MARKER:-}" ]]; then
        /usr/bin/printf '%s\n' "$stage" > "$GNFS_WORKBENCH_PUBLISH_TEST_MARKER"
    fi
    case "${GNFS_WORKBENCH_PUBLISH_TEST_FAULT_MODE:-fail}" in
        fail)
            return 74
            ;;
        term)
            kill -TERM $$
            ;;
        pause)
            sleep "${GNFS_WORKBENCH_PUBLISH_TEST_PAUSE_SECONDS:-1}"
            ;;
        *)
            print -u2 "unknown publish fault mode"
            return 2
            ;;
    esac
}

rollback_file() {
    local name="$1"
    local target="$output_dir/$name"
    local staged="$staged_dir/$name"
    local backup="$backup_dir/$name"

    if [[ -f "$backup" ]]; then
        rm -f -- "$target"
        /bin/mv -f "$backup" "$target"
    elif [[ ! -e "$staged" ]]; then
        rm -f -- "$target"
    fi
}

rollback() {
    if [[ "$publishing" != true || "$committed" == true ]]; then
        return
    fi
    rollback_file "$archive_name"
    rollback_file "$checksum_name"
}

cleanup() {
    local publish_exit_code=$?
    trap - EXIT INT TERM
    if [[ $publish_exit_code -ne 0 ]]; then
        rollback
    fi
    if [[ -n "$transaction_dir" ]]; then
        rm -rf -- "$transaction_dir"
    fi
    rmdir "$lock_dir" 2>/dev/null || true
    exit $publish_exit_code
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

transaction_dir=$(mktemp -d "$output_dir/.gnfs-workbench-publish.XXXXXX")
staged_dir="$transaction_dir/staged"
backup_dir="$transaction_dir/backup"
mkdir -p "$staged_dir" "$backup_dir"
/bin/cp -p "$source_archive" "$staged_dir/$archive_name"
/bin/cp -p "$source_checksum" "$staged_dir/$checksum_name"
verify_checksum_pair \
    "$staged_dir/$archive_name" "$staged_dir/$checksum_name" "$archive_name"

for existing in "$archive_path" "$checksum_path"; do
    if [[ -e "$existing" && (! -f "$existing" || -L "$existing") ]]; then
        print -u2 "refusing to replace non-regular publish target: $existing"
        exit 1
    fi
done
if [[ -f "$archive_path" && ! -f "$checksum_path" ]] ||
    [[ ! -f "$archive_path" && -f "$checksum_path" ]]; then
    print -u2 "refusing to replace an incomplete existing package pair"
    exit 1
fi

publishing=true
if [[ -f "$archive_path" ]]; then
    /bin/mv "$archive_path" "$backup_dir/$archive_name"
fi
fault_point after-backup-archive || exit $?
if [[ -f "$checksum_path" ]]; then
    /bin/mv "$checksum_path" "$backup_dir/$checksum_name"
fi
fault_point after-backup-checksum || exit $?

/bin/mv "$staged_dir/$archive_name" "$archive_path"
fault_point after-new-archive || exit $?

/bin/mv "$staged_dir/$checksum_name" "$checksum_path"
fault_point after-new-checksum || exit $?
verify_checksum_pair "$archive_path" "$checksum_path" "$archive_name"
committed=true
publishing=false

print "$archive_path"
print "$checksum_path"
