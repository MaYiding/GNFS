#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
configuration="${1:-Release}"
output_dir="${2:-$project_root/dist}"
derived_data="/private/tmp/GNFSWorkbenchBuild"
generated_project_root="/private/tmp/GNFSWorkbenchBuildProject"

if [[ "$configuration" != "Debug" && "$configuration" != "Release" ]]; then
    print -u2 "usage: build-app.sh [Debug|Release] [output-directory]"
    exit 2
fi

"$script_dir/generate-project.sh" "$generated_project_root"

xcodebuild -quiet \
    -project "$generated_project_root/GNFSWorkbench.xcodeproj" \
    -scheme GNFSWorkbench \
    -configuration "$configuration" \
    -derivedDataPath "$derived_data" \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM= \
    GNFS_WORKBENCH_SOURCE_ROOT="$project_root" \
    ARCHS=arm64 \
    build

app_path="$derived_data/Build/Products/$configuration/GNFSWorkbench.app"
if [[ ! -d "$app_path" ]]; then
    print -u2 "build completed without producing $app_path"
    exit 1
fi

codesign --verify --deep --strict "$app_path"
mkdir -p "$output_dir"

version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app_path/Contents/Info.plist")
archive_path="$output_dir/GNFSWorkbench-${version}-macOS-arm64.zip"
ditto -c -k --norsrc --keepParent "$app_path" "$archive_path"

print "$archive_path"
