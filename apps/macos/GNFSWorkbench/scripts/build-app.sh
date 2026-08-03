#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
repo_root="${script_dir:A:h:h:h:h}"
configuration="${1:-Release}"
output_dir="${2:-$project_root/dist}"

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

mkdir -p "$output_dir"
archive_name="GNFSWorkbench-${version}-macOS-arm64.zip"
checksum_name="${archive_name}.sha256"
archive_path="$output_dir/$archive_name"
checksum_path="$output_dir/$checksum_name"
archive_temp="$output_dir/.${archive_name}.tmp.$$"
checksum_temp="$output_dir/.${checksum_name}.tmp.$$"

/usr/bin/ditto -c -k --norsrc --keepParent "$app_path" "$archive_temp"
extraction_root="$scratch_root/Extracted"
mkdir -p "$extraction_root"
/usr/bin/ditto -x -k "$archive_temp" "$extraction_root"
verify_app "$extraction_root/GNFSWorkbench.app" "$version"

checksum=$(/usr/bin/shasum -a 256 "$archive_temp" | /usr/bin/cut -d ' ' -f 1)
/usr/bin/printf '%s  %s\n' "$checksum" "$archive_name" > "$checksum_temp"

/bin/mv -f "$archive_temp" "$archive_path"
archive_temp=""
/bin/mv -f "$checksum_temp" "$checksum_path"
checksum_temp=""
(
    cd "$output_dir"
    /usr/bin/shasum -a 256 -c "$checksum_name"
)

print "$archive_path"
print "$checksum_path"
