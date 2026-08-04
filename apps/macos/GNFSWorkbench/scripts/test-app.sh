#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
test_target="GNFSWorkbenchTests"
scratch_root=$(mktemp -d "${TMPDIR:-/private/tmp}/gnfs-workbench-tests.XXXXXX")
derived_data="$scratch_root/DerivedData"
generated_project_root="$scratch_root/Project"
cli_build_root="$scratch_root/CLI"
result_bundle="$scratch_root/TestResults.xcresult"
test_log="$scratch_root/xcodebuild.log"
failure_artifacts="${GNFS_WORKBENCH_TEST_ARTIFACTS:-}"
wall_timeout_seconds="${GNFS_WORKBENCH_TEST_WALL_TIMEOUT_SECONDS:-1200}"
wall_timeout_marker="$scratch_root/wall-timeout"
repo_root="${script_dir:A:h:h:h:h}"
source_revision=$(git -C "$repo_root" rev-parse --verify 'HEAD^{commit}')

cleanup() {
    rm -rf -- "$scratch_root"
}
trap cleanup EXIT INT TERM

preserve_failure_artifacts() {
    [[ -n "$failure_artifacts" ]] || return 0
    mkdir -p "$failure_artifacts"
    if [[ -f "$test_log" ]]; then
        /bin/cp -p "$test_log" "$failure_artifacts/xcodebuild.log"
    fi
    if [[ -d "$result_bundle" ]]; then
        /usr/bin/ditto "$result_bundle" "$failure_artifacts/TestResults.xcresult"
    fi
    if [[ -d "$derived_data/Logs/Test" ]]; then
        /usr/bin/ditto "$derived_data/Logs/Test" "$failure_artifacts/TestLogs"
    fi
    /usr/bin/printf 'target=%s\nrevision=%s\n' \
        "$test_target" "$source_revision" > "$failure_artifacts/context.txt"
}

if [[ "${1:-}" == "--ui" ]]; then
    test_target="GNFSWorkbenchUITests"
elif [[ $# -gt 0 ]]; then
    print -u2 "usage: test-app.sh [--ui]"
    exit 2
fi
if [[ "$wall_timeout_seconds" != <-> || "$wall_timeout_seconds" -lt 1 ]]; then
    print -u2 "GNFS_WORKBENCH_TEST_WALL_TIMEOUT_SECONDS must be a positive integer"
    exit 2
fi

"$script_dir/generate-project.sh" "$generated_project_root"

set +e
GNFS_WORKBENCH_CLI_BUILD_ROOT="$cli_build_root" \
  "$script_dir/run-with-wall-timeout.sh" \
    "$wall_timeout_seconds" "$test_log" "$wall_timeout_marker" -- \
  xcodebuild -quiet \
    -project "$generated_project_root/GNFSWorkbench.xcodeproj" \
    -scheme GNFSWorkbench \
    -configuration Debug \
    -derivedDataPath "$derived_data" \
    -destination 'platform=macOS,arch=arm64' \
    -parallel-testing-enabled NO \
    -test-timeouts-enabled YES \
    -default-test-execution-time-allowance 120 \
    -maximum-test-execution-time-allowance 300 \
    -resultBundlePath "$result_bundle" \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM= \
    GNFS_WORKBENCH_SOURCE_ROOT="$project_root" \
    GNFS_SOURCE_REVISION="$source_revision" \
    -only-testing:"$test_target" \
    test
xcodebuild_exit_code=$?
set -e

if (( xcodebuild_exit_code != 0 )); then
    preserve_failure_artifacts
    exit "$xcodebuild_exit_code"
fi
