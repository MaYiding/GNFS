# Release Process

GNFS publishes releases through the manually dispatched `Release Artifacts`
workflow. The workflow is locked to `v0.1.0` for the first release. A later
release must update the version contract, expected asset names, application
version, and release documentation in one reviewed change.

## Security Boundary

The release workflow accepts only a lowercase, full 40-character commit SHA.
That SHA must satisfy all of these conditions:

- it is the current `origin/main` commit reported by the GitHub API;
- it supplied the workflow definition through `refs/heads/main`;
- every GitHub Actions workflow triggered by that main push completed
  successfully;
- every required job has a successful GitHub Actions check run with the same
  job ID and exact name;
- the release tag and GitHub release do not already exist.

The required contexts include `CI required`, both sanitizer jobs, `Analyze
C++`, static analysis, script checks, workflow security, and `Workbench CI`.
The contract uses the real `Analyze C++` job name rather than the historical
and incorrect `CodeQL` branch-protection context.

The workflow never accepts a branch name, tag, or other mutable checkout ref.
It never overwrites an artifact, tag, release, or uploaded release asset.

## Two-Phase Publication

Run the workflow twice from the `main` branch at the same full SHA.

1. Select `verify-only`, set `release_tag` to `v0.1.0`, provide the current
   main SHA, and type `VERIFY v0.1.0`.
2. Wait for the entire workflow to succeed. This phase runs the reusable
   release qualification, creates the three command-line interface (CLI)
   packages, collects the exact Workbench ZIP tested by the main push, and
   uploads an immutable verification bundle.
3. Select `publish` at the same SHA and type `PUBLISH v0.1.0`.

Publish mode requires the nonexpired artifacts from a completed successful
verify-only run. It downloads and verifies those exact bytes instead of
rebuilding them. It then rechecks the current main SHA, every triggered push
workflow, required job contexts, and unpublished tag state immediately before
creating a draft release. The workflow verifies the draft tag and complete
asset set before making the release public. After all assets have uploaded, a
second API-backed check again verifies current main and every exact-SHA CI run,
then requires the tag to be a lightweight commit ref to that SHA, the draft ID
to equal the ID created by this workflow, and every uploaded asset size and
server-reported SHA-256 digest to equal the verified local bundle. Only then
does the next command change that exact draft to public.

If draft creation or asset upload fails, the workflow does not publish the
draft. An existing draft or tag blocks automatic retry because the workflow
does not delete or replace release state. Inspect and resolve that partial
state explicitly before another publication attempt.

## Qualification Lanes

The reusable release qualification runs only from the scheduled nightly
workflow and verify-only release runs. It does not expand the default pull
request matrix. Its independent Release jobs cover:

- `./scripts/test.sh -t Release thorough`;
- `test_structured_filter_pipeline_120bit`;
- the bounded legacy/structured 50-digit production-route comparison with a
  four-special-Q cap and a 900-second per-route timeout.

The bounded comparison is a structural regression and resource-evidence lane.
It does not claim complete 50-digit factorization.

## Release Assets

The verified bundle contains exactly these files:

- `gnfs-v0.1.0-linux-x86_64.tar.gz`;
- `gnfs-v0.1.0-macos-arm64.tar.gz`;
- `gnfs-v0.1.0-windows-x86_64.zip`;
- `GNFSWorkbench-0.1.0-macOS-arm64.zip`;
- `release-metadata.json`;
- `SHA256SUMS`.

The CLI archive helper sorts paths, normalizes timestamps, ownership, and file
modes, and rejects output replacement. ZIP entries use stored encoding to
avoid compressor-dependent output. The release metadata binds every package
digest to the full source SHA and the source commit epoch.

The Linux x86_64 package is built with GCC 12 inside an Ubuntu 20.04 container,
whose glibc baseline is 2.31. `readelf` must identify an x86-64 executable, only
approved dynamic dependencies, and symbol-version maxima no newer than
`GLIBC_2.31`, `GLIBCXX_3.4.30`, and `CXXABI_1.3.13`. The macOS arm64 package is
configured with `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`; `lipo`, `vtool`, and
`otool` must independently confirm its single architecture and minimum system
version.

The Linux checker writes its observed GLIBC, GLIBCXX, and CXXABI maxima, the
dynamic dependency set, and the executable digest to
`binary-compatibility.json`; it generates `README-release.txt` from those same
values. The archive validator binds the metadata digest to `bin/gnfs`, checks
every observed ABI against the documented ceiling, and requires the exact
minimum versions in the README. A glibc version alone is not a sufficient
compatibility claim because the dynamically linked GCC 12 runtime may require
a newer libstdc++ ABI than the Ubuntu 20.04 default.

All three CLI archive roots contain the repository's byte-identical GPL-2.0
`LICENSE`. Linux and macOS do not bundle GMP or NTL dynamic libraries, and
their README and third-party notice state that the host must provide them.
Windows dependency discovery captures and checks the `ldd` exit status,
rejects unresolved or non-UCRT64 non-system paths, and records the owning
pacman package and version for every copied DLL. License files must resolve
under an MSYS2 license root and are copied into `licenses/`; the archive
validator cross-checks those files, all DLL digests, and
`runtime-dependencies.json`. The executable must also pass its version probe
from the package directory with `/ucrt64/bin` excluded from `PATH`.

`Workbench CI` embeds the full source SHA as `GNFSSourceRevision` in the
application `Info.plist`. The release workflow downloads the ZIP and SHA-256
sidecar from that exact main push workflow run, verifies the sidecar, opens the
ZIP, and checks the embedded source revision and application version. It also
requires the exact six-file `Contents/Resources/Licenses` contract: the
byte-identical GNFS GPL-2.0 license, both GMP copying texts, the NTL copying
notice, a versioned static-link notice, and an upstream source-offer file. A
missing, renamed, or additional file blocks release assembly.

The first Workbench package is ad-hoc signed and is not Apple notarized. macOS
may require an explicit user approval before first launch. The release notes
and `release-metadata.json` disclose this limitation. Do not describe the ZIP
as Developer ID signed or notarized until the workflow has credentials and a
verified notarization step.

## Local Contract Checks

The release checks do not call GitHub during self-test mode:

```bash
python3 scripts/release_contract.py self-test
python3 scripts/release_contract.py check-workflows
python3 scripts/release_binary_contract.py self-test
python3 scripts/reproducible_archive.py self-test
python3 scripts/windows_release_runtime.py self-test
```

Script Checks runs both commands, Python bytecode compilation, and the existing
Harness validation. Workflow Security also runs `actionlint` and `zizmor`
against every workflow change.
