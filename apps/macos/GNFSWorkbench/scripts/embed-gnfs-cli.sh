#!/bin/zsh

set -euo pipefail

if [[ $# -ne 2 ]]; then
    print -u2 "usage: embed-gnfs-cli.sh <destination> <Debug|Release>"
    exit 2
fi

script_dir="${0:A:h}"
repo_root="${script_dir:A:h:h:h:h}"
destination="$1"
configuration="$2"

build_type="Debug"
if [[ "$configuration" == "Release" ]]; then
    build_type="Release"
fi

if ! command -v brew >/dev/null 2>&1; then
    print -u2 "Homebrew is required to locate the static GMP and NTL libraries."
    exit 1
fi

gmp_prefix=$(brew --prefix gmp)
ntl_prefix=$(brew --prefix ntl)
gmp_library="$gmp_prefix/lib/libgmp.a"
ntl_library="$ntl_prefix/lib/libntl.a"

for dependency in "$gmp_library" "$ntl_library"; do
    if [[ ! -f "$dependency" ]]; then
        print -u2 "required static dependency not found: $dependency"
        exit 1
    fi
done

temporary_cli_root=""
if [[ -n "${GNFS_WORKBENCH_CLI_BUILD_ROOT:-}" ]]; then
    cli_build_root="$GNFS_WORKBENCH_CLI_BUILD_ROOT"
elif [[ -n "${TARGET_TEMP_DIR:-}" ]]; then
    cli_build_root="$TARGET_TEMP_DIR/GNFSWorkbenchCLI"
else
    temporary_cli_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-cli.XXXXXX")
    cli_build_root="$temporary_cli_root"
fi

cleanup() {
    if [[ -n "$temporary_cli_root" ]]; then
        rm -rf -- "$temporary_cli_root"
    fi
}
trap cleanup EXIT INT TERM

cli_build_dir="$cli_build_root/$build_type-arm64"
parallel_jobs=$(sysctl -n hw.ncpu 2>/dev/null || print 4)
deployment_target="${MACOSX_DEPLOYMENT_TARGET:-26.0}"

cmake \
    -S "$repo_root" \
    -B "$cli_build_dir" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
    -DGNFS_BUILD_TESTS=OFF \
    -DGNFS_ENABLE_NATIVE_ARCH=OFF \
    -DGMP_INCLUDE_DIR="$gmp_prefix/include" \
    -DGMP_LIBRARY="$gmp_library" \
    -DNTL_INCLUDE_DIR="$ntl_prefix/include" \
    -DNTL_LIBRARY="$ntl_library"

cmake --build "$cli_build_dir" --target gnfs -j"$parallel_jobs"

mkdir -p "${destination:h}"
/usr/bin/install -m 755 "$cli_build_dir/gnfs" "$destination"

codesign_identity="${EXPANDED_CODE_SIGN_IDENTITY:--}"
/usr/bin/codesign \
    --force \
    --options runtime \
    --sign "$codesign_identity" \
    --timestamp=none \
    "$destination"
/usr/bin/codesign --verify --strict "$destination"

print "Embedded self-contained $build_type GNFS CLI at $destination"
