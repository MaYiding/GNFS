#!/bin/zsh

set -euo pipefail

if [[ $# -ne 1 ]]; then
    print -u2 "usage: embed-license-resources.sh <destination-directory>"
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

script_dir="${0:A:h}"
source_dir="${script_dir:A:h}/GNFSWorkbench/Resources/Licenses"
destination_input="$1"
strip_directory_suffixes "$destination_input"
destination_leaf="$REPLY"
if [[ -L "$destination_leaf" ]]; then
    print -u2 "license destination must not be a symbolic link"
    exit 1
fi
destination_dir="${destination_input:A}"
expected_files=(
    GNFS-GPL-2.0.txt
    GMP-COPYING.txt
    GMP-COPYING.LESSERv3.txt
    NTL-copying.txt
    THIRD-PARTY-NOTICES.txt
    SOURCE.txt
)

if [[ ! -d "$source_dir" || -L "$source_dir" ]]; then
    print -u2 "license source must be a real directory: $source_dir"
    exit 1
fi
if [[ -e "$destination_dir" && (! -d "$destination_dir" || -L "$destination_dir") ]]; then
    print -u2 "license destination must be a real directory: $destination_dir"
    exit 1
fi
if [[ $(find "$source_dir" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ') -ne 6 ]] ||
    [[ -n "$(find "$source_dir" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" ]]; then
    print -u2 "license source contract requires exactly six regular files"
    exit 1
fi

mkdir -p "$destination_dir"
for license_name in "${expected_files[@]}"; do
    if [[ ! -s "$source_dir/$license_name" || -L "$source_dir/$license_name" ]]; then
        print -u2 "missing required license source: $license_name"
        exit 1
    fi
    /usr/bin/install -m 0644 \
        "$source_dir/$license_name" "$destination_dir/$license_name"
done

if [[ $(find "$destination_dir" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ') -ne 6 ]] ||
    [[ -n "$(find "$destination_dir" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" ]]; then
    print -u2 "license destination contract requires exactly six regular files"
    exit 1
fi
