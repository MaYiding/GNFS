#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
test_target="GNFSWorkbenchTests"
scratch_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-tests.XXXXXX")
derived_data="$scratch_root/DerivedData"
generated_project_root="$scratch_root/Project"
cli_build_root="$scratch_root/CLI"
repo_root="${script_dir:A:h:h:h:h}"
source_revision=$(git -C "$repo_root" rev-parse --verify 'HEAD^{commit}')

cleanup() {
    rm -rf -- "$scratch_root"
}
trap cleanup EXIT INT TERM

if [[ "${1:-}" == "--ui" ]]; then
    test_target="GNFSWorkbenchUITests"
elif [[ $# -gt 0 ]]; then
    print -u2 "usage: test-app.sh [--ui]"
    exit 2
fi

"$script_dir/generate-project.sh" "$generated_project_root"

GNFS_WORKBENCH_CLI_BUILD_ROOT="$cli_build_root" xcodebuild -quiet \
    -project "$generated_project_root/GNFSWorkbench.xcodeproj" \
    -scheme GNFSWorkbench \
    -configuration Debug \
    -derivedDataPath "$derived_data" \
    -destination 'platform=macOS,arch=arm64' \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM= \
    GNFS_WORKBENCH_SOURCE_ROOT="$project_root" \
    GNFS_SOURCE_REVISION="$source_revision" \
    -only-testing:"$test_target" \
    test
