#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
repo_root="${script_dir:A:h:h:h:h}"
configuration="${1:-Release}"
output_dir_input="${2:-$project_root/dist}"

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

strip_directory_suffixes "$output_dir_input"
output_dir_leaf="$REPLY"
if [[ -L "$output_dir_leaf" ]]; then
    print -u2 "output directory must not be a symbolic link"
    exit 1
fi
output_dir="${output_dir_input:A}"

if [[ "$configuration" != "Debug" && "$configuration" != "Release" ]]; then
    print -u2 "usage: build-app.sh [Debug|Release] [output-directory]"
    exit 2
fi

source_revision=$(git -C "$repo_root" rev-parse --verify 'HEAD^{commit}')
if [[ ! "$source_revision" =~ '^[0-9a-f]{40}$' ]]; then
    print -u2 "unable to resolve the full checkout commit"
    exit 1
fi
if [[ "$configuration" == "Release" ]] &&
    [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=all)" ]]; then
    print -u2 "Release packaging requires a clean checkout so GNFSSourceRevision is exact."
    exit 1
fi

scratch_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-build.XXXXXX")
derived_data="$scratch_root/DerivedData"
generated_project_root="$scratch_root/Project"
cli_build_root="$scratch_root/CLI"
archive_temp=""
checksum_temp=""

cleanup() {
    rm -rf -- "$scratch_root"
    if [[ -n "$archive_temp" && -e "$archive_temp" ]]; then
        rm -f -- "$archive_temp"
    fi
    if [[ -n "$checksum_temp" && -e "$checksum_temp" ]]; then
        rm -f -- "$checksum_temp"
    fi
}
trap cleanup EXIT INT TERM

verify_app() {
    local app_path="$1"
    local expected_version="$2"
    local app_binary="$app_path/Contents/MacOS/GNFSWorkbench"
    local cli_binary="$app_path/Contents/Resources/gnfs"
    local embedded_version
    local embedded_revision
    local architectures
    local dependency_report="$scratch_root/dependencies.txt"
    local event_output="$scratch_root/events.jsonl"
    local event_error="$scratch_root/events.stderr"
    local license_dir="$app_path/Contents/Resources/Licenses"
    local source_license_dir="$project_root/GNFSWorkbench/Resources/Licenses"
    local expected_license_files=(
        GNFS-GPL-2.0.txt
        GMP-COPYING.txt
        GMP-COPYING.LESSERv3.txt
        NTL-copying.txt
        THIRD-PARTY-NOTICES.txt
        SOURCE.txt
    )

    /usr/bin/codesign --verify --deep --strict "$app_path"
    /usr/bin/codesign --verify --strict "$cli_binary"

    architectures=$(/usr/bin/lipo -archs "$app_binary")
    if [[ "$architectures" != "arm64" ]]; then
        print -u2 "unexpected app architectures: $architectures"
        return 1
    fi
    architectures=$(/usr/bin/lipo -archs "$cli_binary")
    if [[ "$architectures" != "arm64" ]]; then
        print -u2 "unexpected CLI architectures: $architectures"
        return 1
    fi

    embedded_version=$(/usr/libexec/PlistBuddy \
        -c 'Print :CFBundleShortVersionString' "$app_path/Contents/Info.plist")
    embedded_revision=$(/usr/libexec/PlistBuddy \
        -c 'Print :GNFSSourceRevision' "$app_path/Contents/Info.plist")
    if [[ "$embedded_version" != "$expected_version" ]]; then
        print -u2 "unexpected bundle version: $embedded_version"
        return 1
    fi
    if [[ "$embedded_revision" != "$source_revision" ]]; then
        print -u2 "bundle source revision does not match checkout HEAD"
        return 1
    fi

    if [[ ! -d "$license_dir" ]]; then
        print -u2 "package is missing the Licenses resource directory"
        return 1
    fi
    if [[ $(find "$license_dir" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ') -ne 6 ]] ||
        [[ -n "$(find "$license_dir" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" ]]; then
        print -u2 "package license resource contract requires exactly six regular files"
        find "$license_dir" -mindepth 1 -maxdepth 1 -print >&2
        return 1
    fi
    for license_name in "${expected_license_files[@]}"; do
        if [[ ! -s "$license_dir/$license_name" ]]; then
            print -u2 "package is missing required license resource: $license_name"
            return 1
        fi
        if ! /usr/bin/cmp -s \
            "$source_license_dir/$license_name" \
            "$license_dir/$license_name"; then
            print -u2 "packaged license resource differs from the committed source: $license_name"
            return 1
        fi
    done
    if ! /usr/bin/cmp -s "$repo_root/LICENSE" "$license_dir/GNFS-GPL-2.0.txt"; then
        print -u2 "packaged GNFS license differs from the repository license"
        return 1
    fi
    if [[ $(/usr/bin/shasum -a 256 "$license_dir/GMP-COPYING.txt" | \
        /usr/bin/cut -d ' ' -f 1) != \
        '8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643' ]]; then
        print -u2 "GMP-COPYING.txt is not the GMP 6.3.0 upstream COPYINGv2 text"
        return 1
    fi
    if ! /usr/bin/grep -Fq 'Version 2, June 1991' \
        "$license_dir/GMP-COPYING.txt"; then
        print -u2 "GMP-COPYING.txt does not identify GNU GPL version 2"
        return 1
    fi
    for required_notice in \
        'GMP 6.3.0' 'NTL 11.6.0' 'statically linked' 'GNU GPL version 2'; do
        if ! /usr/bin/grep -Fq "$required_notice" \
            "$license_dir/THIRD-PARTY-NOTICES.txt"; then
            print -u2 "third-party notice is missing: $required_notice"
            return 1
        fi
    done
    for source_url in \
        'https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz' \
        'https://libntl.org/ntl-11.6.0.tar.gz'; do
        if ! /usr/bin/grep -Fq "$source_url" "$license_dir/SOURCE.txt"; then
            print -u2 "source notice is missing: $source_url"
            return 1
        fi
    done
    for source_contract in \
        'gnfs-v0.1.0-source.tar.gz' \
        'gmp-6.3.0.tar.xz' \
        'ntl-11.6.0.tar.gz' \
        'SHA256SUMS' \
        'a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898' \
        'bc0ef9aceb075a6a0673ac8d8f47d5f8458c72fe806e4468fbd5d3daff056182'; do
        if ! /usr/bin/grep -Fq "$source_contract" "$license_dir/SOURCE.txt"; then
            print -u2 "source release contract is missing: $source_contract"
            return 1
        fi
    done

    /usr/bin/otool -L "$app_binary" > "$dependency_report"
    /usr/bin/otool -L "$cli_binary" >> "$dependency_report"
    if /usr/bin/grep -Eq '/opt/homebrew|/usr/local|/Cellar/' "$dependency_report"; then
        print -u2 "package contains a local Homebrew runtime dependency"
        /bin/cat "$dependency_report" >&2
        return 1
    fi

    (
        cd "$scratch_root"
        env -u GNFS_RESUME -u GNFS_SIEVE_RESUME \
            "$cli_binary" 360 --event-stream --complete --lang en \
            > "$event_output" 2> "$event_error"
    )
    if [[ -s "$event_error" ]]; then
        print -u2 "bundled CLI wrote unexpected stderr during the 360 package probe"
        /bin/cat "$event_error" >&2
        return 1
    fi
    /usr/bin/python3 - "$event_output" <<'PY'
import json
import pathlib
import sys

rows = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
if not rows or any(row.get("schema_version") != 1 for row in rows):
    raise SystemExit("invalid JSONL schema in bundled CLI probe")
results = [row.get("result") for row in rows if row.get("type") == "result"]
if len(results) != 1:
    raise SystemExit("bundled CLI probe did not emit exactly one result")
result = results[0]
if result.get("n") != "360":
    raise SystemExit("bundled CLI probe returned the wrong input")
if result.get("factors") != ["2", "2", "2", "3", "3", "5"]:
    raise SystemExit("bundled CLI probe returned the wrong prime factors")
if result.get("factorization_complete") is not True or result.get("factors_prime") is not True:
    raise SystemExit("bundled CLI probe did not prove complete prime factorization")
PY
}

"$script_dir/generate-project.sh" "$generated_project_root"

GNFS_WORKBENCH_CLI_BUILD_ROOT="$cli_build_root" xcodebuild -quiet \
    -project "$generated_project_root/GNFSWorkbench.xcodeproj" \
    -scheme GNFSWorkbench \
    -configuration "$configuration" \
    -derivedDataPath "$derived_data" \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM= \
    GNFS_WORKBENCH_SOURCE_ROOT="$project_root" \
    GNFS_SOURCE_REVISION="$source_revision" \
    ARCHS=arm64 \
    build

app_path="$derived_data/Build/Products/$configuration/GNFSWorkbench.app"
if [[ ! -d "$app_path" ]]; then
    print -u2 "build completed without producing $app_path"
    exit 1
fi

version=$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleShortVersionString' "$app_path/Contents/Info.plist")
verify_app "$app_path" "$version"

archive_name="GNFSWorkbench-${version}-macOS-arm64.zip"
checksum_name="${archive_name}.sha256"
archive_path="$output_dir/$archive_name"
checksum_path="$output_dir/$checksum_name"
archive_temp="$scratch_root/$archive_name"
checksum_temp="$scratch_root/$checksum_name"

/usr/bin/ditto -c -k --norsrc --keepParent "$app_path" "$archive_temp"
extraction_root="$scratch_root/Extracted"
mkdir -p "$extraction_root"
/usr/bin/ditto -x -k "$archive_temp" "$extraction_root"
verify_app "$extraction_root/GNFSWorkbench.app" "$version"

checksum=$(/usr/bin/shasum -a 256 "$archive_temp" | /usr/bin/cut -d ' ' -f 1)
/usr/bin/printf '%s  %s\n' "$checksum" "$archive_name" > "$checksum_temp"

"$script_dir/publish-package.sh" \
    "$archive_temp" "$checksum_temp" "$output_dir" >/dev/null

print "$archive_path"
print "$checksum_path"
