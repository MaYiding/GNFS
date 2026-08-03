#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
derived_data="/private/tmp/GNFSWorkbenchTests"
generated_project_root="/private/tmp/GNFSWorkbenchTestProject"
test_target="GNFSWorkbenchTests"

if [[ "${1:-}" == "--ui" ]]; then
    test_target="GNFSWorkbenchUITests"
elif [[ $# -gt 0 ]]; then
    print -u2 "usage: test-app.sh [--ui]"
    exit 2
fi

"$script_dir/generate-project.sh" "$generated_project_root"

xcodebuild -quiet \
    -project "$generated_project_root/GNFSWorkbench.xcodeproj" \
    -scheme GNFSWorkbench \
    -configuration Debug \
    -derivedDataPath "$derived_data" \
    -destination 'platform=macOS,arch=arm64' \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM= \
    GNFS_WORKBENCH_SOURCE_ROOT="$project_root" \
    -only-testing:"$test_target" \
    test
