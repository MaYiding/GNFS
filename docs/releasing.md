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
- the release tag does not exist;
- no release claims the tag, except one exact resumable draft for the same
  target SHA.

The required contexts include `CI required`, both sanitizer jobs, static
analysis, script checks, workflow security, and `Workbench CI`. Code scanning
has two independent requirements, and neither substitutes for the other. The
GitHub Actions job `Analyze C++` must match its exact workflow job and check-run
ID. The latest entry in the matching Code Scanning analysis stream must use
`refs/heads/main`, the target SHA, the exact workflow analysis key, category,
and environment, and the `CodeQL` tool. The latest analysis must have an empty
error field and a nonempty tool version. An older successful analysis cannot
hide a newer failed rerun or a newer analysis for a different commit.

Every workflow that supplies a required context has one canonical, unfiltered
`main` push trigger, even when its pull-request trigger retains path filters to
control cost. `check-workflows` accepts only `branches: [ main ]` or
`branches: [ main, dev ]` as the complete push block for those workflows. This
guarantees exactly one eligible push run for every releasable main SHA; a
manually dispatched run cannot substitute for that evidence.

The workflow never accepts a branch name, tag, or other mutable checkout ref.
It never overwrites an artifact, tag, release, or uploaded release asset.

An administrator must enable repository release immutability before the first
publication and retain repository ruleset `20335185` (`Protect release tags`).
The ruleset is active, targets only tags matching `refs/tags/v*`, contains the
`update` and `deletion` rules, and has no bypass actor. The workflow verifies
that exact ruleset before qualification and immediately before publication.
Because GitHub may hide `bypass_actors` from tokens without ruleset-write
access, the contract also pins the ruleset node and its creation/update
instants; any hidden edit changes the pinned version. When `bypass_actors` is
visible, it must be an empty list, and `current_user_can_bypass` must always be
`never`.

GitHub's immutable-release settings endpoint requires repository
`Administration: read`, a permission that is not available to the ephemeral
`GITHUB_TOKEN`. The workflow does not introduce a long-lived personal access
token to bridge that gap. It attempts the read and reports when the setting is
rejected with `403`; a documented `404` means immutability is disabled and
blocks the workflow. A `403` is a residual visibility boundary, not an atomic or
fail-closed proof that the setting stayed enabled. Administrators must run the
strict read-only check with an authenticated admin session immediately before
publish mode and keep the setting enabled throughout the publication window:

```bash
GITHUB_TOKEN="$(gh auth token)" \
  python3 scripts/release_contract.py verify-protection \
  --repository MaYiding/GNFS
```

Release immutability applies only to releases published after the setting is
enabled. Do not dispatch publish mode if that strict administrator check fails.
Do not store an administrator token or personal access token in Actions to
remove this operational boundary. The publish command separately requires its
PATCH response and both post-publication release reads to report
`immutable: true`.

## Two-Phase Publication

Run the workflow twice from the `main` branch at the same full SHA.
The workflow concurrency group is keyed only by release tag, so verify-only and
publish attempts for the same tag cannot overlap even when callers supply
different SHAs.

1. Select `verify-only`, set `release_tag` to `v0.1.0`, provide the current
   main SHA, and type `VERIFY v0.1.0`.
2. Wait for the entire workflow to succeed. This phase runs the reusable
   release qualification, creates the three command-line interface (CLI)
   packages, collects the exact Workbench ZIP tested by the main push, and
   uploads an immutable verification bundle.
3. Select `publish` at the same SHA and type `PUBLISH v0.1.0`.

Publish mode requires the nonexpired artifacts from a completed successful
verify-only run. It downloads and verifies those exact bytes instead of
rebuilding them. The proof records the verify-only workflow run ID, run
attempt, and GitHub URL. These fields must match the run selected by publish
mode.

GitHub does not create a Git tag or tag ref for a draft release. The
`/releases/tags/{tag}` endpoint also does not return drafts. The preparation
command therefore pages through the releases collection, requires at most one
exact tag candidate, and reads an accepted draft by its positive numeric ID.
It reuses only a draft with the exact tag, target SHA, title, notes,
`draft: true`, `prerelease: false`, and `immutable: false` state. The tag ref
must remain absent throughout draft preparation.

Preparation is safe to resume after an interrupted upload. Existing server
assets must be an exact-byte subset of the expected release files, including
matching size and server-reported SHA-256 digest. The command uploads only
missing files. Any extra, duplicate, renamed, incomplete, or wrong-digest asset
blocks the retry. It never deletes, updates, replaces, or clobbers a server
asset. A concurrent draft creation or asset upload converges only when the
observed server object has the same exact identity and bytes.

Immediately before the mutating PATCH, one Python command revalidates the
proof, repository protection, current main, all exact-SHA CI evidence, exact
numeric draft ID, complete asset set, and absent tag. It PATCHes only that draft
ID. GitHub creates the release tag during publication, so only the
post-publication checks require a lightweight commit ref to the target SHA.
After a successful immutable PATCH, the command checks only frozen publication
state: the release by numeric ID and public tag lookup, the exact tag ref, and
the server asset list. A later main-branch or ruleset movement does not turn an
already immutable publication into a reported failure. Success requires the
PATCH response and both release reads to match the exact ID, tag, target,
title, notes, `draft: false`, `prerelease: false`, and `immutable: true`.

A publish retry also accepts one already public release, but only as a recovery
state for the same immutable object. This path revalidates the selected
verify-only proof and all 13 local release files, then requires the numeric-ID
lookup, public tag lookup, lightweight tag ref, title, notes, and every server
asset name, size, and SHA-256 digest to match exactly. It performs no release
creation, asset upload, deletion, or PATCH. This allows a rerun to converge when
the original PATCH succeeded but a transient failure interrupted its first
post-publication read. Recovery depends only on frozen release state, so it does
not require `main` or the ruleset to retain their earlier values. Any conflicting
public identity or asset blocks the retry. Verify-only mode never accepts an
already published release. If `main` has moved, rerun the failed publish
workflow attempt so its original event SHA and inputs remain bound to the
release target; a new dispatch remains intentionally locked to the current
`main` workflow revision.

The verify-only proof embeds the exact Actions workflow, job, and check-run IDs.
It also embeds the selected Code Scanning analysis ID, tool version, timestamp,
result count, ref, commit, analysis key, category, environment, and error field.
Proof validation freezes both the Actions `Analyze C++` contract and the
independent Code Scanning analysis contract. Final prepublish validation
queries both again from the target commit rather than trusting only the earlier
proof.

If draft creation or asset upload fails, the workflow does not publish the
draft. Rerun publish mode with the same tag, target SHA, and verify-only proof to
resume an exact partial draft. A preexisting tag, conflicting release, or
noncanonical asset blocks the retry and requires explicit administrator
inspection. The sole accepted preexisting tag is the exact immutable recovery
state described above. The workflow never repairs conflicting state
destructively.

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

The public release contains exactly these files:

- `gnfs-v0.1.0-linux-x86_64.tar.gz`;
- `gnfs-v0.1.0-macos-arm64.tar.gz`;
- `gnfs-v0.1.0-windows-x86_64.zip`;
- `GNFSWorkbench-0.1.0-macOS-arm64.zip`;
- `gnfs-v0.1.0-source.tar.gz`;
- `gmp-6.3.0.tar.xz`;
- `ntl-11.6.0.tar.gz`;
- `mingw-w64-gcc-16.1.0-6.src.tar.zst`;
- `mingw-w64-gmp-6.3.0-2.src.tar.zst`;
- `mingw-w64-winpthreads-14.0.0.r220.gd999af622-1.src.tar.zst`;
- `release-metadata.json`;
- `SHA256SUMS`;
- `release-verification.json`.

`release-verification.json` is the CI and bundle evidence generated by the
verify-only run. It is uploaded as a public immutable release asset and its
local bytes, size, and server SHA-256 digest are checked before and after
publication. It intentionally does not list its own digest in
`release-metadata.json`, `SHA256SUMS`, or its internal bundle list; doing so
would create a self-reference cycle. Metadata still names it explicitly as
`kind: evidence` with `digest_binding: publication-contract`. The proof binds
every other release asset, while the publication contract separately binds the
proof's local and server-reported digest.

The CLI archive helper sorts paths, normalizes timestamps, ownership, and file
modes, and rejects output replacement. ZIP entries use stored encoding to
avoid compressor-dependent output. The release metadata binds every asset
digest to the full source SHA. The contract recomputes the target commit's Unix
timestamp with Git and requires `source_date_epoch` to equal that value exactly;
a merely positive or caller-selected epoch is rejected.

The verify-only workflow creates `gnfs-v0.1.0-source.tar.gz` directly from the
exact target commit. Its validator requires the Git archive commit marker,
fixed top-level directory, safe paths, and a file-by-file manifest equal to a
fresh archive of that target SHA. The workflow downloads GMP 6.3.0 from two
ordered, fixed, official HTTPS endpoints: `ftp.gnu.org` first and `gmplib.org`
as a fallback. It makes at most three attempts and rotates between those
endpoints. NTL 11.6.0 and the MSYS2 sources retain their fixed HTTPS endpoints.
Every endpoint must return the exact contracted bytes. The workflow accepts
only `gmp-6.3.0.tar.xz` with
SHA-256 `a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898`
and `ntl-11.6.0.tar.gz` with SHA-256
`bc0ef9aceb075a6a0673ac8d8f47d5f8458c72fe806e4468fbd5d3daff056182`.
The same workflow downloads three exact MSYS2 source-only archives for the
Windows runtime closure. Their SHA-256 digests are:

- `mingw-w64-gcc-16.1.0-6.src.tar.zst`:
  `6c24a08c75679601a20bfc4d6c40c34ff5473a89ef69d74f939b2b6f5172327c`;
- `mingw-w64-gmp-6.3.0-2.src.tar.zst`:
  `f288f944fd9609db220bcf6a8dd0703a5674eeb906ef35eb8485bb8192135994`;
- `mingw-w64-winpthreads-14.0.0.r220.gd999af622-1.src.tar.zst`:
  `9743facee4a25c6bb44e856ed8182f7e4c652812481656e68aa57b9070992451`.

These source packages contain the upstream source, `.SRCINFO`, `PKGBUILD`,
and downstream patches used by the selected MSYS2 packages. The GCC contract
enumerates every patch in its source package. All six source archives are
first-class release assets. Their names, sizes, and digests are bound by
`release-metadata.json`, `SHA256SUMS`, the verification proof, and the final
server-side release-asset check. GitHub's generated tag source remains an
additional source path, not a substitute for the verified GNFS source asset.

The Linux x86_64 package is built with GCC 12 inside an Ubuntu 20.04 container,
whose glibc baseline is 2.31. `readelf` must identify an x86-64 executable, only
approved dynamic dependencies, and symbol-version maxima no newer than
`GLIBC_2.31`, `GLIBCXX_3.4.30`, and `CXXABI_1.3.13`. The macOS arm64 package is
configured with `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`; `lipo`, `vtool`, and
`otool` must independently confirm its single architecture and minimum system
version.

The Linux bootstrap uses the Ubuntu Toolchain test PPA through its direct
Focal Deb822 endpoint rather than the Launchpad metadata API. The source is
limited by `Signed-By` to the PPA key whose full fingerprint is
`C8EC952E2A0E1FBDC5090F6A2C277A0A352154E5`; key retrieval and APT transport
have bounded retries and timeouts. The job also checks amd64, glibc 2.31, and
GCC/G++ major version 12 before building. CMake 3.31.6 is installed only from
the exact PyPI `manylinux2014_x86_64` wheel whose SHA-256 is
`1c8b05df0602365da91ee6a3336fe57525b137706c4ab5675498f662ae1dbcec`.
Pip runs with `--require-hashes`, `--only-binary=:all:`, and `--no-deps`, then
the job checks the installed CMake version. The wheel is build infrastructure,
not a release asset. Because GitHub mounts the workspace into the pinned
container with host ownership, the job registers only `${GITHUB_WORKSPACE}` as
a Git safe directory before verifying the exact SHA and clean tree. This
authenticates the rolling PPA and enforces the binary ABI ceiling, but it is not
a bit-for-bit pin of the complete Debian package closure.

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
Windows uses a frozen MSYS2 UCRT64 baseline rather than rolling package names.
The workflow downloads digest-pinned GCC 16.1.0-6, GCC runtime, GMP 6.3.0-2,
and winpthreads packages from `repo.msys2.org`, verifies their package
archives, and installs those exact versions. NTL is not installed for this
package because GNFS currently imports no NTL symbol; the release configuration
also sets `GNFS_ENABLE_NTL=OFF`, so a pre-existing library cannot silently enter
the link. The runtime closure is
exactly `libgmp-10.dll`, `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, and
`libwinpthread-1.dll`, owned by three packages. Any missing or additional DLL,
package, version, digest, or source mapping fails closed.

Windows dependency discovery captures and checks the `ldd` exit status,
rejects unresolved or non-UCRT64 non-system paths, and records the owning
package, binary package digest, DLL digest, and corresponding source archive.
Package license files are copied into `licenses/`. The MSYS2 GMP binary package
contains no license file, so the packager copies the byte-pinned upstream
`COPYINGv2` text and explicitly conveys GMP under its GNU GPL version 2 option.
The archive validator cross-checks every license, runtime contract, DLL, and
source mapping. The executable must also pass its version probe from the
package directory with `/ucrt64/bin` excluded from `PATH`.

`Release Readiness` repeats the complete pinned Windows build, isolated launch,
archive creation, and archive validation on every pull request and `main` push.
It deliberately has no path filter because it is a required check and the release
preflight requires exact-SHA evidence in addition to
the ordinary Windows compiler matrix.

`Workbench CI` embeds the full source SHA as `GNFSSourceRevision` in the
application `Info.plist`. The release workflow downloads the ZIP and SHA-256
sidecar from that exact main push workflow run, verifies the sidecar, opens the
ZIP, and checks the embedded source revision and application version. It also
requires the exact six-file `Contents/Resources/Licenses` contract: the
byte-identical GNFS GPL-2.0 license, both GMP copying texts, the NTL copying
notice, a versioned static-link notice, and a source-location file. A
missing, renamed, or additional file blocks release assembly.
`GMP-COPYING.txt` is pinned byte-for-byte to GMP 6.3.0 `COPYINGv2` and must
carry the GPL version 2 header, while the third-party notice explicitly selects
GMP's GNU GPL version 2 option for this distribution. The corresponding-source
URLs are version-locked to the GMP 6.3.0 and NTL 11.6.0 archives.

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
python3 scripts/windows_runtime_contract.py self-test
```

Script Checks runs both commands, Python bytecode compilation, and the existing
Harness validation. Workflow Security also runs `actionlint` and `zizmor`
against every workflow change.

The first release records exact source revisions, platform-specific binary
dependency contracts, corresponding sources, and CI evidence. It does not
publish a separate SPDX/CycloneDX SBOM, a detached GPG/Sigstore signature, or a
signed SLSA-style toolchain provenance statement for each compiler and
packaging tool. The current dependency/source closure is auditable but is not a
standards-format SBOM or an independent publisher signature. These are future
hardening items, not claims made by `v0.1.0`.

Adding any such artifact later requires adding its exact name and identity to
the release asset contract, metadata, checksum manifest, verification proof,
self-tests, and final server-side closure check. Do not upload an unbound
sidecar asset or generate a nominal SBOM that invents host-provided Linux or
macOS GMP/NTL versions.
