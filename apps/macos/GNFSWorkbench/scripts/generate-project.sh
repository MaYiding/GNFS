#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_root="${script_dir:A:h}"
output_root="${1:-$project_root}"

if ! command -v xcodegen >/dev/null 2>&1; then
    print -u2 "XcodeGen is required. Install it with: brew install xcodegen"
    exit 1
fi

mkdir -p "$output_root"
xcodegen generate \
    --spec "$project_root/project.yml" \
    --project "$output_root" \
    --project-root "$project_root"
print "Generated ${output_root}/GNFSWorkbench.xcodeproj"
