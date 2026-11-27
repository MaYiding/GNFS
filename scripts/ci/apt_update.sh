#!/usr/bin/env bash

# Refresh the APT index on a GitHub-hosted Ubuntu runner before a workflow
# installs its build dependencies.
#
# The runner image ships APT sources for packages.microsoft.com. No workflow in
# this repository installs from them, and when that host answers 403 the
# failure aborts apt-get update with exit code 100 before any build starts.
# apt does not retry a 403, so drop every source file that points at the host
# first. One-line .list files and deb822 .sources files share one directory,
# which is why the removal selects files by content instead of by name.

set -euo pipefail

sources_dir=/etc/apt/sources.list.d

# grep exits with 1 when no file matches, which is a valid state here.
removed=0
while IFS= read -r -d '' source_file; do
    sudo rm -f -- "${source_file}"
    printf 'Removed APT source file that points at packages.microsoft.com: %s\n' \
        "${source_file}"
    removed=$((removed + 1))
done < <(sudo grep -rl --null 'packages.microsoft.com' "${sources_dir}" || true)

if ((removed == 0)); then
    printf 'No APT source file under %s points at packages.microsoft.com.\n' "${sources_dir}"
fi

sudo apt-get -o Acquire::Retries=3 update
