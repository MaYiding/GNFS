#!/usr/bin/env python3
"""Validate GNFS release inputs, CI evidence, artifacts, and publication proofs."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import ssl
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import Any, Callable, Protocol
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen
import zipfile


FIRST_RELEASE_TAG = "v0.1.0"
FULL_SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
REPOSITORY_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
RELEASE_WORKFLOW_PATH = ".github/workflows/release.yml"
WORKBENCH_ARTIFACT_NAME = "gnfs-workbench-macos-arm64"
WORKBENCH_INFO_KEY = "GNFSSourceRevision"
LINUX_ABI_CEILINGS = {
    "GLIBC": "2.31",
    "GLIBCXX": "3.4.30",
    "CXXABI": "1.3.13",
}
WORKBENCH_LICENSE_RESOURCES = {
    "GNFS-GPL-2.0.txt": ("project",),
    "GMP-COPYING.txt": ("GNU GENERAL PUBLIC LICENSE", "Version 2, June 1991"),
    "GMP-COPYING.LESSERv3.txt": ("GNU LESSER GENERAL PUBLIC LICENSE",),
    "NTL-copying.txt": ("NTL -- A Library for Doing Number Theory",),
    "THIRD-PARTY-NOTICES.txt": (
        "GMP 6.3.0",
        "NTL 11.6.0",
        "statically linked",
        "GNU GPL version 2",
    ),
    "SOURCE.txt": (
        "https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz",
        "https://libntl.org/ntl-11.6.0.tar.gz",
    ),
}
WORKBENCH_LICENSE_FORBIDDEN_MARKERS = {
    "GMP-COPYING.txt": ("Version 3, 29 June 2007",),
}
WORKBENCH_LICENSE_SHA256 = {
    "GMP-COPYING.txt": "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643",
}


@dataclass(frozen=True)
class RequiredCheck:
    workflow: str
    workflow_path: str
    job: str


@dataclass(frozen=True)
class RequiredExternalCheck:
    name: str
    app_id: int
    app_slug: str


REQUIRED_MAIN_CHECKS = (
    RequiredCheck("CI", ".github/workflows/ci.yml", "CI required"),
    RequiredCheck(
        "Sanitizers", ".github/workflows/sanitizers.yml", "AddressSanitizer + UBSanitizer"
    ),
    RequiredCheck(
        "Sanitizers",
        ".github/workflows/sanitizers.yml",
        "ThreadSanitizer (candidate and structured relation)",
    ),
    RequiredCheck("CodeQL", ".github/workflows/codeql.yml", "Analyze C++"),
    RequiredCheck("Static Analysis", ".github/workflows/static-analysis.yml", "clang-tidy"),
    RequiredCheck(
        "Static Analysis", ".github/workflows/static-analysis.yml", "workflow YAML lint"
    ),
    RequiredCheck("Script Checks", ".github/workflows/scripts.yml", "zsh/bash syntax"),
    RequiredCheck(
        "Workflow Security", ".github/workflows/workflow-security.yml", "actionlint"
    ),
    RequiredCheck(
        "Workflow Security",
        ".github/workflows/workflow-security.yml",
        "zizmor CI/CD security scan",
    ),
    RequiredCheck(
        "Workbench CI",
        ".github/workflows/workbench.yml",
        "macOS 26 build, test, and package",
    ),
)
REQUIRED_EXTERNAL_CHECKS = (
    RequiredExternalCheck("CodeQL", 57789, "github-advanced-security"),
)


class ReleaseContractError(RuntimeError):
    """Raised when release evidence violates a fail-closed contract."""


class GitHubAPI(Protocol):
    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        """Return one GitHub API response."""

    def get_optional(self, path: str) -> Any | None:
        """Return one response or None for HTTP 404."""

    def paginate(self, path: str, key: str, query: dict[str, str] | None = None) -> list[Any]:
        """Return every item from a paginated GitHub API response."""


class GitHubClient:
    def __init__(self, token: str, api_url: str = "https://api.github.com") -> None:
        if not token:
            raise ReleaseContractError("GITHUB_TOKEN is required for release verification")
        self._token = token
        self._api_url = api_url.rstrip("/")
        self._ssl_context = _verified_ssl_context()

    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        suffix = path if path.startswith("/") else f"/{path}"
        url = f"{self._api_url}{suffix}"
        if query:
            url = f"{url}?{urlencode(query)}"
        request = Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "gnfs-release-contract/1",
            },
        )
        try:
            with urlopen(request, timeout=30, context=self._ssl_context) as response:
                return json.load(response)
        except HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")[:1000]
            raise ReleaseContractError(
                f"GitHub API {error.code} for {path}: {detail}"
            ) from error
        except (URLError, TimeoutError, json.JSONDecodeError) as error:
            raise ReleaseContractError(f"GitHub API request failed for {path}: {error}") from error

    def get_optional(self, path: str) -> Any | None:
        suffix = path if path.startswith("/") else f"/{path}"
        url = f"{self._api_url}{suffix}"
        request = Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "gnfs-release-contract/1",
            },
        )
        try:
            with urlopen(request, timeout=30, context=self._ssl_context) as response:
                return json.load(response)
        except HTTPError as error:
            if error.code == 404:
                return None
            detail = error.read().decode("utf-8", errors="replace")[:1000]
            raise ReleaseContractError(
                f"GitHub API {error.code} for {path}: {detail}"
            ) from error
        except (URLError, TimeoutError, json.JSONDecodeError) as error:
            raise ReleaseContractError(f"GitHub API request failed for {path}: {error}") from error

    def paginate(self, path: str, key: str, query: dict[str, str] | None = None) -> list[Any]:
        items: list[Any] = []
        page = 1
        while True:
            page_query = dict(query or {})
            page_query.update({"per_page": "100", "page": str(page)})
            payload = self.get(path, page_query)
            page_items = payload.get(key)
            if not isinstance(page_items, list):
                raise ReleaseContractError(f"GitHub API response has no list field {key!r}")
            items.extend(page_items)
            if len(page_items) < 100:
                return items
            page += 1
            if page > 100:
                raise ReleaseContractError(f"GitHub API pagination exceeded 100 pages for {path}")


def _verified_ssl_context() -> ssl.SSLContext:
    default_paths = ssl.get_default_verify_paths()
    if default_paths.cafile or default_paths.capath:
        return ssl.create_default_context()
    for candidate in (
        Path("/etc/ssl/cert.pem"),
        Path("/etc/ssl/certs/ca-certificates.crt"),
    ):
        if candidate.is_file():
            return ssl.create_default_context(cafile=str(candidate))
    return ssl.create_default_context()


def _validate_sha(value: str, label: str = "target SHA") -> None:
    if not FULL_SHA_PATTERN.fullmatch(value):
        raise ReleaseContractError(f"{label} must be a canonical lowercase 40-hex SHA")


def _validate_repository(repository: str) -> None:
    if not REPOSITORY_PATTERN.fullmatch(repository):
        raise ReleaseContractError("repository must use the owner/name form")


def validate_dispatch(
    mode: str,
    release_tag: str,
    target_sha: str,
    confirmation: str,
    repository: str,
    workflow_ref: str,
    workflow_sha: str,
) -> None:
    if mode not in {"verify-only", "publish"}:
        raise ReleaseContractError("mode must be verify-only or publish")
    if release_tag != FIRST_RELEASE_TAG:
        raise ReleaseContractError(
            f"the first-release workflow is locked to {FIRST_RELEASE_TAG}; update the contract "
            "explicitly for a later version"
        )
    _validate_sha(target_sha)
    _validate_sha(workflow_sha, "workflow SHA")
    _validate_repository(repository)
    expected_confirmation = (
        f"VERIFY {release_tag}" if mode == "verify-only" else f"PUBLISH {release_tag}"
    )
    if confirmation != expected_confirmation:
        raise ReleaseContractError(
            f"confirmation must be exactly {expected_confirmation!r} for mode {mode}"
        )
    expected_workflow_ref = f"{repository}/{RELEASE_WORKFLOW_PATH}@refs/heads/main"
    if workflow_ref != expected_workflow_ref:
        raise ReleaseContractError(
            "release workflow must be dispatched from refs/heads/main; "
            f"received {workflow_ref!r}"
        )
    if workflow_sha != target_sha:
        raise ReleaseContractError(
            "target SHA must equal the SHA that supplied the main-branch workflow definition"
        )


def verify_checkout(target_sha: str, repository_root: Path) -> None:
    _validate_sha(target_sha)
    root = repository_root.resolve()
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, check=True, capture_output=True, text=True
    ).stdout.strip()
    if head != target_sha:
        raise ReleaseContractError(f"checkout HEAD is {head}, expected {target_sha}")
    status = subprocess.run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    if status:
        raise ReleaseContractError("exact-SHA checkout is not clean before release work")


def _assert_unpublished(client: GitHubAPI, repository: str, release_tag: str) -> None:
    encoded_tag = quote(release_tag, safe="")
    if client.get_optional(f"/repos/{repository}/git/ref/tags/{encoded_tag}") is not None:
        raise ReleaseContractError(f"refusing to reuse existing tag {release_tag}")
    if client.get_optional(f"/repos/{repository}/releases/tags/{encoded_tag}") is not None:
        raise ReleaseContractError(f"refusing to reuse existing release {release_tag}")


def _verify_required_external_checks(check_runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    for required in REQUIRED_EXTERNAL_CHECKS:
        matches = [check for check in check_runs if check.get("name") == required.name]
        if len(matches) != 1:
            raise ReleaseContractError(
                f"expected exactly one external {required.name!r} check run; found {len(matches)}"
            )
        check = matches[0]
        check_id = check.get("id")
        app = check.get("app") or {}
        if not isinstance(check_id, int) or check_id <= 0:
            raise ReleaseContractError(
                f"external {required.name!r} check run has no positive numeric id"
            )
        if (
            check.get("status") != "completed"
            or check.get("conclusion") != "success"
            or app.get("id") != required.app_id
            or app.get("slug") != required.app_slug
        ):
            raise ReleaseContractError(
                f"external {required.name!r} check is not a successful "
                f"{required.app_slug} app context"
            )
        evidence.append(
            {
                "name": required.name,
                "check_run_id": check_id,
                "app_id": required.app_id,
                "app_slug": required.app_slug,
                "status": "completed",
                "conclusion": "success",
            }
        )
    return evidence


def verify_main_ci(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    require_unpublished: bool = True,
) -> dict[str, Any]:
    _validate_repository(repository)
    _validate_sha(target_sha)
    if release_tag != FIRST_RELEASE_TAG:
        raise ReleaseContractError(f"release tag must be {FIRST_RELEASE_TAG}")

    main_ref = client.get(f"/repos/{repository}/git/ref/heads/main")
    main_sha = main_ref.get("object", {}).get("sha")
    if main_sha != target_sha:
        raise ReleaseContractError(
            f"target SHA {target_sha} is not the current origin/main SHA {main_sha}"
        )
    if require_unpublished:
        _assert_unpublished(client, repository, release_tag)

    runs = client.paginate(
        f"/repos/{repository}/actions/runs",
        "workflow_runs",
        {"head_sha": target_sha, "branch": "main", "event": "push"},
    )
    exact_runs = [
        run
        for run in runs
        if run.get("head_sha") == target_sha
        and run.get("head_branch") == "main"
        and run.get("event") == "push"
    ]
    if not exact_runs:
        raise ReleaseContractError("no main push workflow runs exist for the target SHA")
    bad_runs = [
        run
        for run in exact_runs
        if run.get("status") != "completed" or run.get("conclusion") != "success"
    ]
    if bad_runs:
        summary = ", ".join(
            f"{run.get('name')}={run.get('status')}/{run.get('conclusion')}"
            for run in sorted(bad_runs, key=lambda item: str(item.get("name")))
        )
        raise ReleaseContractError(f"not every triggered main push workflow passed: {summary}")

    runs_by_identity: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for run in exact_runs:
        identity = (str(run.get("name")), str(run.get("path")))
        runs_by_identity.setdefault(identity, []).append(run)

    check_runs = client.paginate(
        f"/repos/{repository}/commits/{target_sha}/check-runs",
        "check_runs",
        {"filter": "all"},
    )
    checks_by_id = {check.get("id"): check for check in check_runs}
    jobs_by_run: dict[int, list[dict[str, Any]]] = {}
    evidence: list[dict[str, Any]] = []
    workbench_run_id: int | None = None

    for required in REQUIRED_MAIN_CHECKS:
        identity = (required.workflow, required.workflow_path)
        candidates = runs_by_identity.get(identity, [])
        if len(candidates) != 1:
            raise ReleaseContractError(
                f"expected exactly one {required.workflow} main push run at {required.workflow_path}; "
                f"found {len(candidates)}"
            )
        run = candidates[0]
        run_id = run.get("id")
        if not isinstance(run_id, int):
            raise ReleaseContractError(f"workflow run {required.workflow} has no numeric id")
        if run_id not in jobs_by_run:
            jobs_by_run[run_id] = client.paginate(
                f"/repos/{repository}/actions/runs/{run_id}/jobs", "jobs"
            )
        matching_jobs = [job for job in jobs_by_run[run_id] if job.get("name") == required.job]
        if len(matching_jobs) != 1:
            raise ReleaseContractError(
                f"expected exactly one job context {required.job!r} in {required.workflow}; "
                f"found {len(matching_jobs)}"
            )
        job = matching_jobs[0]
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            raise ReleaseContractError(
                f"required job {required.job!r} did not pass: "
                f"{job.get('status')}/{job.get('conclusion')}"
            )
        job_id = job.get("id")
        check = checks_by_id.get(job_id)
        if not check:
            raise ReleaseContractError(
                f"required job {required.job!r} has no exact commit check run with id {job_id}"
            )
        if (
            check.get("name") != required.job
            or check.get("status") != "completed"
            or check.get("conclusion") != "success"
            or check.get("app", {}).get("slug") != "github-actions"
        ):
            raise ReleaseContractError(
                f"check run {required.job!r} is not a successful GitHub Actions context"
            )
        evidence.append(
            {
                "workflow": required.workflow,
                "workflow_path": required.workflow_path,
                "workflow_run_id": run_id,
                "job": required.job,
                "check_run_id": job_id,
            }
        )
        if required.workflow == "Workbench CI":
            workbench_run_id = run_id

    if workbench_run_id is None:
        raise ReleaseContractError("Workbench CI run id was not resolved")
    external_evidence = _verify_required_external_checks(check_runs)
    return {
        "schema_version": 2,
        "target_sha": target_sha,
        "all_triggered_push_workflows": len(exact_runs),
        "required_checks": evidence,
        "required_external_checks": external_evidence,
        "workbench_run_id": workbench_run_id,
    }


def _artifact_name(prefix: str, release_tag: str, target_sha: str) -> str:
    return f"{prefix}-{release_tag}-{target_sha}"


def find_verification_run(
    client: GitHubAPI, repository: str, target_sha: str, release_tag: str
) -> int:
    _validate_repository(repository)
    _validate_sha(target_sha)
    proof_name = _artifact_name("release-verification", release_tag, target_sha)
    assets_name = _artifact_name("release-assets", release_tag, target_sha)
    artifacts = client.paginate(
        f"/repos/{repository}/actions/artifacts", "artifacts", {"name": proof_name}
    )
    candidates: list[tuple[int, int]] = []
    for artifact in artifacts:
        if artifact.get("name") != proof_name or artifact.get("expired") is not False:
            continue
        workflow_run = artifact.get("workflow_run") or {}
        run_id = workflow_run.get("id")
        if workflow_run.get("head_sha") != target_sha or not isinstance(run_id, int):
            continue
        run = client.get(f"/repos/{repository}/actions/runs/{run_id}")
        if (
            run.get("name") != "Release Artifacts"
            or run.get("path") != RELEASE_WORKFLOW_PATH
            or run.get("event") != "workflow_dispatch"
            or run.get("head_branch") != "main"
            or run.get("head_sha") != target_sha
            or run.get("status") != "completed"
            or run.get("conclusion") != "success"
        ):
            continue
        artifact_id = artifact.get("id")
        if isinstance(artifact_id, int):
            candidates.append((artifact_id, run_id))
    if not candidates:
        raise ReleaseContractError(
            "publish mode requires a completed successful verify-only artifact for the exact SHA"
        )
    _, selected_run_id = max(candidates)
    run_artifacts = client.paginate(
        f"/repos/{repository}/actions/runs/{selected_run_id}/artifacts", "artifacts"
    )
    matching_assets = [
        artifact
        for artifact in run_artifacts
        if artifact.get("name") == assets_name and artifact.get("expired") is False
    ]
    if len(matching_assets) != 1:
        raise ReleaseContractError(
            f"verification run {selected_run_id} must contain one nonexpired {assets_name} artifact"
        )
    return selected_run_id


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_package_names(release_tag: str) -> tuple[str, ...]:
    if release_tag != FIRST_RELEASE_TAG:
        raise ReleaseContractError(f"release tag must be {FIRST_RELEASE_TAG}")
    version = release_tag[1:]
    return (
        f"GNFSWorkbench-{version}-macOS-arm64.zip",
        f"gnfs-{release_tag}-linux-x86_64.tar.gz",
        f"gnfs-{release_tag}-macos-arm64.tar.gz",
        f"gnfs-{release_tag}-windows-x86_64.zip",
    )


def _validate_safe_zip(archive: zipfile.ZipFile) -> None:
    names: set[str] = set()
    if len(archive.infolist()) > 10_000:
        raise ReleaseContractError("ZIP contains too many entries")
    for info in archive.infolist():
        path = PurePosixPath(info.filename)
        if info.filename in names:
            raise ReleaseContractError(f"ZIP contains a duplicate path: {info.filename}")
        names.add(info.filename)
        if (
            path.is_absolute()
            or ".." in path.parts
            or not path.parts
            or "\\" in info.filename
            or "\0" in info.filename
        ):
            raise ReleaseContractError(f"ZIP contains an unsafe path: {info.filename}")
        entry_mode = info.external_attr >> 16
        if stat.S_ISLNK(entry_mode):
            raise ReleaseContractError(f"ZIP contains a symlink: {info.filename}")


def _project_license() -> bytes:
    license_path = Path(__file__).resolve().parents[1] / "LICENSE"
    try:
        content = license_path.read_bytes()
    except OSError as error:
        raise ReleaseContractError(f"unable to read project LICENSE: {error}") from error
    if b"GNU GENERAL PUBLIC LICENSE" not in content or b"Version 2" not in content:
        raise ReleaseContractError("project LICENSE is not the expected GPL-2.0 text")
    return content


def _validate_workbench_zip(zip_path: Path, target_sha: str, release_tag: str) -> None:
    _validate_sha(target_sha)
    expected_name = expected_package_names(release_tag)[0]
    if zip_path.name != expected_name:
        raise ReleaseContractError(f"unexpected Workbench ZIP name: {zip_path.name}")
    try:
        with zipfile.ZipFile(zip_path) as archive:
            _validate_safe_zip(archive)
            plist_paths = [
                name
                for name in archive.namelist()
                if name.endswith("GNFSWorkbench.app/Contents/Info.plist")
                and not name.startswith("__MACOSX/")
            ]
            if len(plist_paths) != 1:
                raise ReleaseContractError(
                    "Workbench ZIP must contain exactly one app Info.plist"
                )
            plist_entry = archive.getinfo(plist_paths[0])
            if plist_entry.file_size > 1024 * 1024:
                raise ReleaseContractError("Workbench Info.plist exceeds the 1 MiB contract cap")
            info = plistlib.loads(archive.read(plist_paths[0]))
            resource_prefix = "GNFSWorkbench.app/Contents/Resources/Licenses/"
            expected_resources = {
                f"{resource_prefix}{name}": markers
                for name, markers in WORKBENCH_LICENSE_RESOURCES.items()
            }
            observed_resources = {
                name
                for name in archive.namelist()
                if name.startswith(resource_prefix) and not name.endswith("/")
            }
            if observed_resources != set(expected_resources):
                raise ReleaseContractError(
                    "Workbench license resource set is missing, extra, or renamed: "
                    f"{sorted(observed_resources)}"
                )
            for resource_path, markers in expected_resources.items():
                try:
                    entry = archive.getinfo(resource_path)
                except KeyError as error:
                    raise ReleaseContractError(
                        f"Workbench ZIP is missing fixed license resource {resource_path}"
                    ) from error
                if entry.is_dir() or entry.file_size == 0 or entry.file_size > 2 * 1024 * 1024:
                    raise ReleaseContractError(
                        f"Workbench license resource has an invalid size: {resource_path}"
                    )
                resource = archive.read(resource_path)
                resource_name = PurePosixPath(resource_path).name
                if markers == ("project",):
                    if resource != _project_license():
                        raise ReleaseContractError(
                            "Workbench project license does not match the repository LICENSE"
                        )
                else:
                    forbidden_markers = WORKBENCH_LICENSE_FORBIDDEN_MARKERS.get(
                        resource_name, ()
                    )
                    present_forbidden_markers = [
                        marker for marker in forbidden_markers if marker.encode() in resource
                    ]
                    if present_forbidden_markers:
                        raise ReleaseContractError(
                            "Workbench license resource contains forbidden identity markers "
                            f"{present_forbidden_markers}: {resource_path}"
                        )
                    missing_markers = [
                        marker for marker in markers if marker.encode() not in resource
                    ]
                    if missing_markers:
                        raise ReleaseContractError(
                            "Workbench license resource lacks identity markers "
                            f"{missing_markers}: {resource_path}"
                        )
                    expected_digest = WORKBENCH_LICENSE_SHA256.get(resource_name)
                    if (
                        expected_digest is not None
                        and hashlib.sha256(resource).hexdigest() != expected_digest
                    ):
                        raise ReleaseContractError(
                            "Workbench license resource is not the pinned upstream text: "
                            f"{resource_path}"
                        )
    except (zipfile.BadZipFile, KeyError, ValueError, plistlib.InvalidFileException) as error:
        raise ReleaseContractError(f"invalid Workbench ZIP: {error}") from error
    if info.get(WORKBENCH_INFO_KEY) != target_sha:
        raise ReleaseContractError(
            f"Workbench {WORKBENCH_INFO_KEY} does not match target SHA"
        )
    if info.get("CFBundleShortVersionString") != release_tag[1:]:
        raise ReleaseContractError("Workbench version does not match release tag")


def validate_workbench_artifact(
    artifact_directory: Path, target_sha: str, release_tag: str
) -> Path:
    artifact_directory = artifact_directory.resolve()
    expected_zip_name = expected_package_names(release_tag)[0]
    expected_sidecar_name = f"{expected_zip_name}.sha256"
    entries = list(artifact_directory.rglob("*"))
    if any(path.is_symlink() for path in entries):
        raise ReleaseContractError("Workbench artifact may not contain symlinks")
    files = sorted(
        path.relative_to(artifact_directory).as_posix() for path in entries if path.is_file()
    )
    if files != sorted((expected_zip_name, expected_sidecar_name)):
        raise ReleaseContractError(
            "Workbench artifact must contain only the canonical ZIP and SHA256 sidecar; "
            f"found {files}"
        )
    zip_path = artifact_directory / expected_zip_name
    sidecar = artifact_directory / expected_sidecar_name
    sidecar_text = sidecar.read_text(encoding="utf-8")
    match = re.fullmatch(
        rf"([0-9a-f]{{64}})  {re.escape(expected_zip_name)}\n", sidecar_text
    )
    if not match:
        raise ReleaseContractError("Workbench SHA256 sidecar is not canonical shasum output")
    if match.group(1) != _sha256(zip_path):
        raise ReleaseContractError("Workbench ZIP checksum does not match its sidecar")
    _validate_workbench_zip(zip_path, target_sha, release_tag)
    return zip_path


def _safe_archive_path(name: str, expected_root: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if (
        not path.parts
        or path.is_absolute()
        or ".." in path.parts
        or "\\" in name
        or "\0" in name
        or path.parts[0] != expected_root
    ):
        raise ReleaseContractError(f"CLI archive contains an unsafe or unexpected path: {name}")
    return path


def _decode_archive_text(read_file: Callable[[str], bytes], name: str) -> str:
    content = read_file(name)
    if not content or len(content) > 8 * 1024 * 1024:
        raise ReleaseContractError(f"CLI archive text file has an invalid size: {name}")
    try:
        return content.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ReleaseContractError(f"CLI archive text file is not UTF-8: {name}") from error


def _validate_windows_runtime_manifest(
    root: str, files: set[str], read_file: Callable[[str], bytes]
) -> None:
    manifest_name = f"{root}/runtime-dependencies.json"
    try:
        manifest = json.loads(_decode_archive_text(read_file, manifest_name))
    except json.JSONDecodeError as error:
        raise ReleaseContractError(f"Windows runtime manifest is invalid JSON: {error}") from error
    if not isinstance(manifest, dict) or set(manifest) != {
        "dependencies",
        "runtime",
        "schema_version",
    }:
        raise ReleaseContractError("Windows runtime manifest has missing or unknown fields")
    if manifest.get("schema_version") != 1 or manifest.get("runtime") != "MSYS2 UCRT64":
        raise ReleaseContractError("Windows runtime manifest identity is invalid")
    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        raise ReleaseContractError("Windows runtime manifest has no bundled dependencies")
    observed_dlls: list[str] = []
    declared_licenses: set[str] = set()
    for dependency in dependencies:
        if not isinstance(dependency, dict) or set(dependency) != {
            "dll",
            "license_files",
            "package",
            "package_version",
            "sha256",
        }:
            raise ReleaseContractError("Windows runtime dependency has missing or unknown fields")
        dll = dependency.get("dll")
        package = dependency.get("package")
        package_version = dependency.get("package_version")
        digest = dependency.get("sha256")
        licenses = dependency.get("license_files")
        if (
            not isinstance(dll, str)
            or PurePosixPath(dll).name != dll
            or not dll.lower().endswith(".dll")
            or not isinstance(package, str)
            or not re.fullmatch(r"[A-Za-z0-9@+_.-]+", package)
            or not isinstance(package_version, str)
            or not package_version
            or any(character.isspace() for character in package_version)
            or not isinstance(digest, str)
            or not SHA256_PATTERN.fullmatch(digest)
            or not isinstance(licenses, list)
            or not licenses
            or not all(isinstance(license_path, str) for license_path in licenses)
            or licenses != sorted(set(licenses))
        ):
            raise ReleaseContractError(f"Windows runtime dependency record is invalid: {dll!r}")
        dll_path = f"{root}/bin/{dll}"
        if dll_path not in files or hashlib.sha256(read_file(dll_path)).hexdigest() != digest:
            raise ReleaseContractError(f"Windows runtime DLL digest mismatch: {dll}")
        for license_path in licenses:
            if (
                not isinstance(license_path, str)
                or not license_path.startswith(f"licenses/{package}/")
                or PurePosixPath(license_path).name in {"", ".", ".."}
            ):
                raise ReleaseContractError(f"Windows runtime license path is invalid: {license_path}")
            rooted_license = f"{root}/{license_path}"
            if rooted_license not in files or not read_file(rooted_license):
                raise ReleaseContractError(f"Windows runtime license is missing or empty: {license_path}")
            declared_licenses.add(rooted_license)
        observed_dlls.append(dll)
    if observed_dlls != sorted(set(observed_dlls), key=str.lower):
        raise ReleaseContractError("Windows runtime dependencies are duplicate or unsorted")
    archived_dlls = {
        PurePosixPath(name).name
        for name in files
        if name.startswith(f"{root}/bin/") and name.lower().endswith(".dll")
    }
    if archived_dlls != set(observed_dlls):
        raise ReleaseContractError("Windows archive DLL set does not match its runtime manifest")
    archived_licenses = {name for name in files if name.startswith(f"{root}/licenses/")}
    if archived_licenses != declared_licenses:
        raise ReleaseContractError(
            "Windows archive license set does not match its runtime manifest"
        )
    notice = _decode_archive_text(read_file, f"{root}/THIRD_PARTY_NOTICES.txt")
    if "runtime-dependencies.json" not in notice:
        raise ReleaseContractError("Windows third-party notice does not identify its manifest")
    for dependency in dependencies:
        identity = f"{dependency['package']} {dependency['package_version']}"
        if identity not in notice:
            raise ReleaseContractError(
                f"Windows third-party notice omits package identity {identity}"
            )


def _version_tuple(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", value):
        raise ReleaseContractError(f"invalid numeric ABI version: {value!r}")
    return tuple(int(component) for component in value.split("."))


def _validate_linux_binary_metadata(
    root: str,
    files: set[str],
    read_file: Callable[[str], bytes],
    readme: str,
    notice: str,
) -> None:
    metadata_name = f"{root}/binary-compatibility.json"
    if metadata_name not in files:
        raise ReleaseContractError("Linux CLI archive lacks binary-compatibility.json")
    try:
        metadata = json.loads(_decode_archive_text(read_file, metadata_name))
    except json.JSONDecodeError as error:
        raise ReleaseContractError(f"Linux binary metadata is invalid JSON: {error}") from error
    expected_keys = {
        "abi_ceilings",
        "abi_requirements",
        "architecture",
        "binary_sha256",
        "dynamic_dependencies",
        "platform",
        "schema_version",
    }
    if not isinstance(metadata, dict) or set(metadata) != expected_keys:
        raise ReleaseContractError("Linux binary metadata has missing or unknown fields")
    if (
        metadata.get("schema_version") != 1
        or metadata.get("platform") != "linux-x86_64"
        or metadata.get("architecture") != "x86_64"
        or metadata.get("abi_ceilings") != LINUX_ABI_CEILINGS
    ):
        raise ReleaseContractError("Linux binary metadata identity or ceilings changed")
    requirements = metadata.get("abi_requirements")
    if not isinstance(requirements, dict) or set(requirements) != set(LINUX_ABI_CEILINGS):
        raise ReleaseContractError("Linux binary metadata has incomplete ABI requirements")
    for family, ceiling in LINUX_ABI_CEILINGS.items():
        requirement = requirements.get(family)
        if not isinstance(requirement, str) or _version_tuple(requirement) > _version_tuple(ceiling):
            raise ReleaseContractError(f"Linux {family} requirement exceeds its release ceiling")
        if f"{family}_{requirement}" not in readme:
            raise ReleaseContractError(
                f"Linux release README omits its exact {family} requirement"
            )
    binary = read_file(f"{root}/bin/gnfs")
    if metadata.get("binary_sha256") != hashlib.sha256(binary).hexdigest():
        raise ReleaseContractError("Linux binary metadata digest does not match bin/gnfs")
    dependencies = metadata.get("dynamic_dependencies")
    if (
        not isinstance(dependencies, list)
        or not dependencies
        or not all(isinstance(item, str) for item in dependencies)
        or dependencies != sorted(set(dependencies))
    ):
        raise ReleaseContractError("Linux dynamic dependency metadata is empty or noncanonical")
    approved_patterns = (
        r"libc\.so\.\d+",
        r"libdl\.so\.\d+",
        r"libgcc_s\.so\.\d+",
        r"libgmp(?:xx)?\.so\.\d+",
        r"libm\.so\.\d+",
        r"libntl\.so\.\d+",
        r"libpthread\.so\.\d+",
        r"librt\.so\.\d+",
        r"libstdc\+\+\.so\.\d+",
    )
    unexpected = [
        dependency
        for dependency in dependencies
        if not any(re.fullmatch(pattern, dependency) for pattern in approved_patterns)
    ]
    if unexpected:
        raise ReleaseContractError(
            f"Linux binary metadata has unexpected dynamic dependencies: {unexpected}"
        )
    for document_name, document in (("README", readme), ("notice", notice)):
        if "binary-compatibility.json" not in document or not all(
            family in document for family in LINUX_ABI_CEILINGS
        ):
            raise ReleaseContractError(
                f"Linux release {document_name} omits its machine-generated ABI metadata"
            )


def _validate_cli_archive_contents(
    root: str,
    files: set[str],
    read_file: Callable[[str], bytes],
    platform: str,
) -> None:
    license_name = f"{root}/LICENSE"
    if license_name not in files or read_file(license_name) != _project_license():
        raise ReleaseContractError("CLI archive project LICENSE is missing or does not match")
    required = {
        license_name,
        f"{root}/README-release.txt",
        f"{root}/THIRD_PARTY_NOTICES.txt",
    }
    executable_name = f"{root}/bin/gnfs.exe" if platform == "windows-x86_64" else f"{root}/bin/gnfs"
    required.add(executable_name)
    missing = sorted(required - files)
    if missing:
        raise ReleaseContractError(f"CLI archive is missing required files: {missing}")
    if not read_file(executable_name):
        raise ReleaseContractError("CLI archive executable is empty")

    readme = _decode_archive_text(read_file, f"{root}/README-release.txt")
    notice = _decode_archive_text(read_file, f"{root}/THIRD_PARTY_NOTICES.txt")
    if platform == "windows-x86_64":
        _validate_windows_runtime_manifest(root, files, read_file)
        if "UCRT64" not in readme or "runtime-dependencies.json" not in readme:
            raise ReleaseContractError("Windows release README omits bundled-runtime provenance")
        return
    if platform == "linux-x86_64":
        _validate_linux_binary_metadata(root, files, read_file, readme, notice)
    bundled_dynamic = [
        name
        for name in files
        if re.search(r"\.so(?:\.|$)", name.lower()) or name.lower().endswith(".dylib")
    ]
    if bundled_dynamic:
        raise ReleaseContractError(
            f"POSIX CLI archive unexpectedly bundles dynamic libraries: {bundled_dynamic}"
        )
    for label, document in (("README", readme), ("third-party notice", notice)):
        lowered = document.lower()
        if not all(marker in lowered for marker in ("gmp", "ntl", "not bundled")):
            raise ReleaseContractError(
                f"POSIX release {label} must state that GMP and NTL dynamic libraries are not bundled"
            )


def validate_cli_archive(archive_path: Path, platform: str, release_tag: str) -> None:
    package_names = expected_package_names(release_tag)
    expected_by_platform = {
        "linux-x86_64": package_names[1],
        "macos-arm64": package_names[2],
        "windows-x86_64": package_names[3],
    }
    expected_name = expected_by_platform.get(platform)
    if expected_name is None:
        raise ReleaseContractError(f"unsupported CLI release platform: {platform}")
    archive_path = archive_path.resolve()
    if archive_path.name != expected_name or not archive_path.is_file() or archive_path.is_symlink():
        raise ReleaseContractError(
            f"CLI archive path does not match the {platform} contract: {archive_path.name}"
        )
    root = (
        expected_name[: -len(".tar.gz")]
        if expected_name.endswith(".tar.gz")
        else expected_name[: -len(".zip")]
    )

    if expected_name.endswith(".tar.gz"):
        try:
            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = archive.getmembers()
                if not members or len(members) > 20_000:
                    raise ReleaseContractError("CLI tar archive has an invalid entry count")
                names: set[str] = set()
                regular: dict[str, tarfile.TarInfo] = {}
                for member in members:
                    normalized = member.name.rstrip("/")
                    _safe_archive_path(normalized, root)
                    if normalized in names:
                        raise ReleaseContractError(f"CLI tar archive has a duplicate path: {normalized}")
                    names.add(normalized)
                    if member.issym() or member.islnk():
                        raise ReleaseContractError(f"CLI tar archive contains a link: {normalized}")
                    if member.isfile():
                        regular[normalized] = member
                    elif not member.isdir():
                        raise ReleaseContractError(
                            f"CLI tar archive contains an unsupported entry: {normalized}"
                        )

                def read_tar(name: str) -> bytes:
                    member = regular.get(name)
                    if member is None:
                        raise ReleaseContractError(f"CLI tar archive lacks regular file {name}")
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise ReleaseContractError(f"unable to read CLI tar member {name}")
                    with extracted:
                        return extracted.read()

                _validate_cli_archive_contents(root, set(regular), read_tar, platform)
        except tarfile.TarError as error:
            raise ReleaseContractError(f"invalid CLI tar archive: {error}") from error
        return

    try:
        with zipfile.ZipFile(archive_path) as archive:
            _validate_safe_zip(archive)
            regular = {
                info.filename: info for info in archive.infolist() if not info.is_dir()
            }
            for name in regular:
                _safe_archive_path(name, root)

            def read_zip(name: str) -> bytes:
                try:
                    return archive.read(name)
                except KeyError as error:
                    raise ReleaseContractError(f"CLI ZIP lacks regular file {name}") from error

            _validate_cli_archive_contents(root, set(regular), read_zip, platform)
    except zipfile.BadZipFile as error:
        raise ReleaseContractError(f"invalid CLI ZIP archive: {error}") from error


def _asset_record(path: Path, kind: str, platform: str) -> dict[str, Any]:
    return {
        "name": path.name,
        "kind": kind,
        "platform": platform,
        "sha256": _sha256(path),
        "size": path.stat().st_size,
    }


def _flat_file_names(directory: Path, label: str) -> list[str]:
    if not directory.is_dir() or directory.is_symlink():
        raise ReleaseContractError(f"{label} must be a real directory")
    entries = list(directory.iterdir())
    invalid = [entry.name for entry in entries if entry.is_symlink() or not entry.is_file()]
    if invalid:
        raise ReleaseContractError(f"{label} contains non-file entries: {sorted(invalid)}")
    return sorted(entry.name for entry in entries)


def assemble_release_bundle(
    asset_directory: Path, target_sha: str, release_tag: str, source_date_epoch: int
) -> None:
    _validate_sha(target_sha)
    if source_date_epoch <= 0:
        raise ReleaseContractError("source date epoch must be a positive integer")
    asset_directory = asset_directory.resolve()
    package_names = expected_package_names(release_tag)
    existing = _flat_file_names(asset_directory, "release assembly directory")
    if existing != sorted(package_names):
        raise ReleaseContractError(
            f"release assembly requires exactly the four package assets; found {existing}"
        )
    _validate_workbench_zip(asset_directory / package_names[0], target_sha, release_tag)
    for platform, name in zip(
        ("linux-x86_64", "macos-arm64", "windows-x86_64"), package_names[1:]
    ):
        validate_cli_archive(asset_directory / name, platform, release_tag)

    records = [
        _asset_record(asset_directory / package_names[0], "macos-application", "macos-arm64"),
        _asset_record(asset_directory / package_names[1], "cli-sdk", "linux-x86_64"),
        _asset_record(asset_directory / package_names[2], "cli-sdk", "macos-arm64"),
        _asset_record(asset_directory / package_names[3], "cli-sdk", "windows-x86_64"),
    ]
    records.sort(key=lambda record: record["name"])
    metadata = {
        "schema_version": 1,
        "release_tag": release_tag,
        "target_sha": target_sha,
        "source_date_epoch": source_date_epoch,
        "assets": records,
        "workbench_security": {
            "signing": "ad-hoc",
            "notarized": False,
            "source_revision_key": WORKBENCH_INFO_KEY,
        },
    }
    metadata_path = asset_directory / "release-metadata.json"
    checksums_path = asset_directory / "SHA256SUMS"
    if metadata_path.exists() or checksums_path.exists():
        raise ReleaseContractError("refusing to overwrite release metadata or checksums")
    with metadata_path.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(metadata, handle, indent=2, sort_keys=True)
        handle.write("\n")

    checksum_paths = [asset_directory / name for name in package_names]
    checksum_paths.append(metadata_path)
    with checksums_path.open("x", encoding="utf-8", newline="\n") as handle:
        for path in sorted(checksum_paths, key=lambda item: item.name):
            handle.write(f"{_sha256(path)}  {path.name}\n")
    verify_release_bundle(asset_directory, target_sha, release_tag)


def _read_json_object(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReleaseContractError(f"invalid JSON file {path.name}: {error}") from error
    if not isinstance(payload, dict):
        raise ReleaseContractError(f"JSON file {path.name} must contain an object")
    return payload


def verify_release_bundle(asset_directory: Path, target_sha: str, release_tag: str) -> None:
    _validate_sha(target_sha)
    asset_directory = asset_directory.resolve()
    package_names = expected_package_names(release_tag)
    expected_files = sorted((*package_names, "release-metadata.json", "SHA256SUMS"))
    files = _flat_file_names(asset_directory, "release bundle directory")
    if files != expected_files:
        raise ReleaseContractError(
            f"release bundle has an unexpected file set: {files}; expected {expected_files}"
        )
    _validate_workbench_zip(asset_directory / package_names[0], target_sha, release_tag)
    for platform, name in zip(
        ("linux-x86_64", "macos-arm64", "windows-x86_64"), package_names[1:]
    ):
        validate_cli_archive(asset_directory / name, platform, release_tag)

    metadata = _read_json_object(asset_directory / "release-metadata.json")
    expected_metadata_keys = {
        "schema_version",
        "release_tag",
        "target_sha",
        "source_date_epoch",
        "assets",
        "workbench_security",
    }
    if set(metadata) != expected_metadata_keys:
        raise ReleaseContractError("release metadata contains missing or unknown fields")
    if (
        metadata.get("schema_version") != 1
        or metadata.get("release_tag") != release_tag
        or metadata.get("target_sha") != target_sha
        or not isinstance(metadata.get("source_date_epoch"), int)
        or metadata["source_date_epoch"] <= 0
    ):
        raise ReleaseContractError("release metadata identity is invalid")
    if metadata.get("workbench_security") != {
        "signing": "ad-hoc",
        "notarized": False,
        "source_revision_key": WORKBENCH_INFO_KEY,
    }:
        raise ReleaseContractError("release metadata must disclose ad-hoc, unnotarized Workbench")

    assets = metadata.get("assets")
    if not isinstance(assets, list) or len(assets) != len(package_names):
        raise ReleaseContractError("release metadata must describe exactly four package assets")
    asset_names = [asset.get("name") for asset in assets if isinstance(asset, dict)]
    if asset_names != sorted(package_names):
        raise ReleaseContractError("release metadata assets are not canonical and sorted")
    for asset in assets:
        if set(asset) != {"name", "kind", "platform", "sha256", "size"}:
            raise ReleaseContractError("release metadata asset contains unknown fields")
        path = asset_directory / asset["name"]
        if (
            not SHA256_PATTERN.fullmatch(str(asset["sha256"]))
            or asset["sha256"] != _sha256(path)
            or asset["size"] != path.stat().st_size
        ):
            raise ReleaseContractError(f"release metadata digest mismatch for {path.name}")

    checksum_path = asset_directory / "SHA256SUMS"
    expected_checksum_names = sorted((*package_names, "release-metadata.json"))
    checksum_lines = checksum_path.read_text(encoding="utf-8").splitlines()
    if len(checksum_lines) != len(expected_checksum_names):
        raise ReleaseContractError("SHA256SUMS has the wrong number of records")
    observed_names: list[str] = []
    for line in checksum_lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)", line)
        if not match:
            raise ReleaseContractError("SHA256SUMS contains a noncanonical record")
        digest, name = match.groups()
        path = asset_directory / name
        if not path.is_file() or _sha256(path) != digest:
            raise ReleaseContractError(f"SHA256SUMS digest mismatch for {name}")
        observed_names.append(name)
    if observed_names != expected_checksum_names:
        raise ReleaseContractError("SHA256SUMS records are missing, extra, or unsorted")


def _required_check_contract() -> list[dict[str, str]]:
    return [asdict(required) for required in REQUIRED_MAIN_CHECKS]


def _required_external_check_contract() -> list[dict[str, Any]]:
    return [asdict(required) for required in REQUIRED_EXTERNAL_CHECKS]


def _validate_main_ci_evidence(evidence: dict[str, Any], target_sha: str) -> None:
    expected_keys = {
        "schema_version",
        "target_sha",
        "all_triggered_push_workflows",
        "required_checks",
        "required_external_checks",
        "workbench_run_id",
    }
    if set(evidence) != expected_keys:
        raise ReleaseContractError("main CI evidence contains missing or unknown fields")
    if (
        evidence.get("schema_version") != 2
        or evidence.get("target_sha") != target_sha
        or not isinstance(evidence.get("all_triggered_push_workflows"), int)
        or evidence["all_triggered_push_workflows"] <= 0
        or not isinstance(evidence.get("workbench_run_id"), int)
        or evidence["workbench_run_id"] <= 0
    ):
        raise ReleaseContractError("main CI evidence identity is invalid")
    required_checks = evidence.get("required_checks")
    if not isinstance(required_checks, list) or len(required_checks) != len(REQUIRED_MAIN_CHECKS):
        raise ReleaseContractError("main CI evidence has the wrong Actions check count")
    workbench_run_id: int | None = None
    for required, record in zip(REQUIRED_MAIN_CHECKS, required_checks):
        if not isinstance(record, dict) or set(record) != {
            "workflow",
            "workflow_path",
            "workflow_run_id",
            "job",
            "check_run_id",
        }:
            raise ReleaseContractError("main CI Actions evidence record is malformed")
        if (
            record.get("workflow") != required.workflow
            or record.get("workflow_path") != required.workflow_path
            or record.get("job") != required.job
            or not isinstance(record.get("workflow_run_id"), int)
            or record["workflow_run_id"] <= 0
            or not isinstance(record.get("check_run_id"), int)
            or record["check_run_id"] <= 0
        ):
            raise ReleaseContractError("main CI Actions evidence changed identity")
        if required.workflow == "Workbench CI":
            workbench_run_id = record["workflow_run_id"]
    if workbench_run_id != evidence["workbench_run_id"]:
        raise ReleaseContractError("main CI evidence has an inconsistent Workbench run id")

    external_checks = evidence.get("required_external_checks")
    if not isinstance(external_checks, list) or len(external_checks) != len(
        REQUIRED_EXTERNAL_CHECKS
    ):
        raise ReleaseContractError("main CI evidence has the wrong external check count")
    for required, record in zip(REQUIRED_EXTERNAL_CHECKS, external_checks):
        if not isinstance(record, dict) or set(record) != {
            "name",
            "check_run_id",
            "app_id",
            "app_slug",
            "status",
            "conclusion",
        }:
            raise ReleaseContractError("main CI external evidence record is malformed")
        if (
            record.get("name") != required.name
            or record.get("app_id") != required.app_id
            or record.get("app_slug") != required.app_slug
            or record.get("status") != "completed"
            or record.get("conclusion") != "success"
            or not isinstance(record.get("check_run_id"), int)
            or record["check_run_id"] <= 0
        ):
            raise ReleaseContractError("main CI external evidence changed identity or result")


def write_verification_proof(
    asset_directory: Path,
    output: Path,
    ci_evidence_path: Path,
    target_sha: str,
    release_tag: str,
) -> None:
    verify_release_bundle(asset_directory, target_sha, release_tag)
    ci_evidence = _read_json_object(ci_evidence_path)
    _validate_main_ci_evidence(ci_evidence, target_sha)
    output = output.resolve()
    if output.exists() or output.is_symlink():
        raise ReleaseContractError(f"refusing to overwrite verification proof: {output}")
    bundle_names = sorted(
        (*expected_package_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    proof = {
        "schema_version": 2,
        "mode": "verify-only",
        "release_tag": release_tag,
        "target_sha": target_sha,
        "required_main_checks": _required_check_contract(),
        "required_external_checks": _required_external_check_contract(),
        "main_ci_evidence": ci_evidence,
        "bundle": [
            {
                "name": name,
                "sha256": _sha256(asset_directory / name),
                "size": (asset_directory / name).stat().st_size,
            }
            for name in bundle_names
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(proof, handle, indent=2, sort_keys=True)
        handle.write("\n")


def verify_verification_proof(
    proof_path: Path, asset_directory: Path, target_sha: str, release_tag: str
) -> None:
    verify_release_bundle(asset_directory, target_sha, release_tag)
    proof = _read_json_object(proof_path)
    expected_keys = {
        "schema_version",
        "mode",
        "release_tag",
        "target_sha",
        "required_main_checks",
        "required_external_checks",
        "main_ci_evidence",
        "bundle",
    }
    if set(proof) != expected_keys:
        raise ReleaseContractError("verification proof contains missing or unknown fields")
    if (
        proof.get("schema_version") != 2
        or proof.get("mode") != "verify-only"
        or proof.get("release_tag") != release_tag
        or proof.get("target_sha") != target_sha
        or proof.get("required_main_checks") != _required_check_contract()
        or proof.get("required_external_checks") != _required_external_check_contract()
    ):
        raise ReleaseContractError("verification proof identity or required checks changed")
    main_ci_evidence = proof.get("main_ci_evidence")
    if not isinstance(main_ci_evidence, dict):
        raise ReleaseContractError("verification proof lacks main CI evidence")
    _validate_main_ci_evidence(main_ci_evidence, target_sha)
    expected_names = sorted(
        (*expected_package_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    bundle = proof.get("bundle")
    if not isinstance(bundle, list) or [entry.get("name") for entry in bundle] != expected_names:
        raise ReleaseContractError("verification proof bundle is missing, extra, or unsorted")
    for entry in bundle:
        if set(entry) != {"name", "sha256", "size"}:
            raise ReleaseContractError("verification proof asset contains unknown fields")
        path = asset_directory / entry["name"]
        if entry["sha256"] != _sha256(path) or entry["size"] != path.stat().st_size:
            raise ReleaseContractError(f"verification proof mismatch for {path.name}")


def final_prepublish(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    expected_release_id: int,
    asset_directory: Path,
) -> dict[str, Any]:
    """Revalidate mutable GitHub state after upload and immediately before publication."""

    if expected_release_id <= 0:
        raise ReleaseContractError("expected draft release id must be positive")
    verify_release_bundle(asset_directory, target_sha, release_tag)
    ci_evidence = verify_main_ci(
        client,
        repository,
        target_sha,
        release_tag,
        require_unpublished=False,
    )
    encoded_tag = quote(release_tag, safe="")
    tag_ref = client.get(f"/repos/{repository}/git/ref/tags/{encoded_tag}")
    if (
        tag_ref.get("ref") != f"refs/tags/{release_tag}"
        or tag_ref.get("object", {}).get("type") != "commit"
        or tag_ref.get("object", {}).get("sha") != target_sha
    ):
        raise ReleaseContractError("release tag is not an exact lightweight ref to target SHA")

    release = client.get(f"/repos/{repository}/releases/tags/{encoded_tag}")
    if (
        release.get("id") != expected_release_id
        or release.get("draft") is not True
        or release.get("prerelease") is not False
        or release.get("tag_name") != release_tag
        or release.get("target_commitish") != target_sha
    ):
        raise ReleaseContractError(
            "release is not the exact target-SHA draft created by this workflow"
        )
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ReleaseContractError("draft release API response has no asset list")
    expected_names = sorted(
        (*expected_package_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    if len(assets) != len(expected_names):
        raise ReleaseContractError("draft release has the wrong number of assets")
    assets_by_name: dict[str, dict[str, Any]] = {}
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise ReleaseContractError("draft release contains an invalid asset record")
        name = asset["name"]
        if name in assets_by_name:
            raise ReleaseContractError(f"draft release contains duplicate asset {name}")
        assets_by_name[name] = asset
    if sorted(assets_by_name) != expected_names:
        raise ReleaseContractError("draft release asset set does not match verified bundle")
    for name in expected_names:
        path = asset_directory / name
        asset = assets_by_name[name]
        if (
            asset.get("state") != "uploaded"
            or asset.get("size") != path.stat().st_size
            or asset.get("digest") != f"sha256:{_sha256(path)}"
        ):
            raise ReleaseContractError(
                f"draft release asset bytes do not match verified bundle: {name}"
            )
    return {
        "schema_version": 1,
        "release_id": expected_release_id,
        "release_tag": release_tag,
        "target_sha": target_sha,
        "asset_count": len(expected_names),
        "ci": ci_evidence,
    }


def validate_workflow_sources(release_workflow: Path, qualification_workflow: Path) -> None:
    release_text = release_workflow.read_text(encoding="utf-8")
    qualification_text = qualification_workflow.read_text(encoding="utf-8")
    forbidden_release_fragments = (
        "if: always()",
        "--clobber",
        "continue-on-error:",
        "github.ref }}",
        "github.ref_name",
        "inputs.tag",
        "mapfile -t runtime_dlls",
    )
    for fragment in forbidden_release_fragments:
        if fragment in release_text:
            raise ReleaseContractError(
                f"release workflow contains forbidden mutable or fail-open fragment: {fragment}"
            )
    if re.search(r"(?m)^\s{2}push:\s*$", release_text) or re.search(
        r"(?m)^\s+tags:\s*$", release_text
    ):
        raise ReleaseContractError("release workflow must not have a push or tag trigger")
    required_release_fragments = (
        "workflow_dispatch:",
        "- verify-only",
        "- publish",
        "PUBLISH v0.1.0",
        "scripts/release_contract.py verify-main",
        "scripts/release_contract.py find-verification",
        "scripts/release_contract.py verify-proof",
        "scripts/release_contract.py final-prepublish",
        "release-main-ci-evidence-${{ github.run_id }}",
        "--ci-evidence main-ci-evidence/main-ci-evidence.json",
        "scripts/release_binary_contract.py linux",
        "scripts/release_binary_contract.py macos",
        "scripts/windows_release_runtime.py bundle",
        "container: ubuntu:20.04",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0",
        "release-verification-${{ inputs.release_tag }}-${{ inputs.target_sha }}",
        "GNFSWorkbench-0.1.0-macOS-arm64.zip",
        "--draft",
        "--method PATCH",
        "releases/${EXPECTED_RELEASE_ID}",
    )
    for fragment in required_release_fragments:
        if fragment not in release_text:
            raise ReleaseContractError(f"release workflow lost required boundary: {fragment}")
    forbidden_qualification_triggers = ("workflow_dispatch:", "pull_request:", "schedule:")
    for trigger in forbidden_qualification_triggers:
        if trigger in qualification_text:
            raise ReleaseContractError(
                f"reusable release qualification gained a direct trigger: {trigger}"
            )
    if "workflow_call:" not in qualification_text:
        raise ReleaseContractError("release qualification must remain workflow_call-only")
    if "if: always()" in qualification_text or "continue-on-error:" in qualification_text:
        raise ReleaseContractError("release qualification contains a fail-open job boundary")


def _write_github_output(path: Path | None, key: str, value: str | int) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write(f"{key}={value}\n")


class _FakeClient:
    def __init__(self, target_sha: str) -> None:
        self.target_sha = target_sha
        self.runs: list[dict[str, Any]] = []
        self.jobs: dict[int, list[dict[str, Any]]] = {}
        self.checks: list[dict[str, Any]] = []
        run_id = 1000
        job_id = 5000
        grouped: dict[tuple[str, str], list[str]] = {}
        for required in REQUIRED_MAIN_CHECKS:
            grouped.setdefault((required.workflow, required.workflow_path), []).append(required.job)
        for (workflow, workflow_path), job_names in grouped.items():
            run_id += 1
            self.runs.append(
                {
                    "id": run_id,
                    "name": workflow,
                    "path": workflow_path,
                    "event": "push",
                    "head_branch": "main",
                    "head_sha": target_sha,
                    "status": "completed",
                    "conclusion": "success",
                }
            )
            self.jobs[run_id] = []
            for job_name in job_names:
                job_id += 1
                job = {
                    "id": job_id,
                    "name": job_name,
                    "status": "completed",
                    "conclusion": "success",
                }
                self.jobs[run_id].append(job)
                self.checks.append({**job, "app": {"slug": "github-actions"}})
        self.runs.append(
            {
                "id": 2000,
                "name": "Optional Success",
                "path": ".github/workflows/optional.yml",
                "event": "push",
                "head_branch": "main",
                "head_sha": target_sha,
                "status": "completed",
                "conclusion": "success",
            }
        )
        self.external_check_id = 9000
        self.checks.append(
            {
                "id": self.external_check_id,
                "name": "CodeQL",
                "status": "completed",
                "conclusion": "success",
                "app": {
                    "id": 57789,
                    "slug": "github-advanced-security",
                    "name": "GitHub Advanced Security",
                },
            }
        )
        self.proof_artifacts: list[dict[str, Any]] = []
        self.run_details: dict[int, dict[str, Any]] = {}
        self.run_artifacts: dict[int, list[dict[str, Any]]] = {}
        self.tag_ref: dict[str, Any] | None = None
        self.release: dict[str, Any] | None = None

    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        del query
        if path.endswith("/git/ref/heads/main"):
            return {"object": {"sha": self.target_sha}}
        if "/git/ref/tags/" in path and self.tag_ref is not None:
            return self.tag_ref
        if "/releases/tags/" in path and self.release is not None:
            return self.release
        match = re.search(r"/actions/runs/(\d+)$", path)
        if match:
            return self.run_details[int(match.group(1))]
        raise AssertionError(f"unexpected fake GET {path}")

    def get_optional(self, path: str) -> Any | None:
        if "/git/ref/tags/" in path or "/releases/tags/" in path:
            return None
        raise AssertionError(f"unexpected fake optional GET {path}")

    def paginate(self, path: str, key: str, query: dict[str, str] | None = None) -> list[Any]:
        del key
        if path.endswith("/actions/runs"):
            return self.runs
        match = re.search(r"/actions/runs/(\d+)/jobs$", path)
        if match:
            return self.jobs[int(match.group(1))]
        if path.endswith(f"/commits/{self.target_sha}/check-runs"):
            if query != {"filter": "all"}:
                raise AssertionError("external check verification must request every check run")
            return self.checks
        if path.endswith("/actions/artifacts") and query:
            return self.proof_artifacts
        match = re.search(r"/actions/runs/(\d+)/artifacts$", path)
        if match:
            return self.run_artifacts[int(match.group(1))]
        raise AssertionError(f"unexpected fake pagination {path}")


def _make_workbench_artifact(directory: Path, target_sha: str) -> Path:
    zip_name = expected_package_names(FIRST_RELEASE_TAG)[0]
    zip_path = directory / zip_name
    project_license = _project_license()
    gmp_copying_v2 = b"                    " + project_license
    if hashlib.sha256(gmp_copying_v2).hexdigest() != WORKBENCH_LICENSE_SHA256[
        "GMP-COPYING.txt"
    ]:
        raise ReleaseContractError(
            "self-test could not derive the pinned GMP 6.3.0 COPYINGv2 fixture"
        )
    info = {
        "CFBundleShortVersionString": FIRST_RELEASE_TAG[1:],
        WORKBENCH_INFO_KEY: target_sha,
    }
    with zipfile.ZipFile(zip_path, mode="x") as archive:
        archive.writestr(
            "GNFSWorkbench.app/Contents/Info.plist",
            plistlib.dumps(info, fmt=plistlib.FMT_BINARY),
        )
        archive.writestr("GNFSWorkbench.app/Contents/MacOS/GNFSWorkbench", b"binary")
        license_prefix = "GNFSWorkbench.app/Contents/Resources/Licenses"
        archive.writestr(f"{license_prefix}/GNFS-GPL-2.0.txt", project_license)
        archive.writestr(
            f"{license_prefix}/GMP-COPYING.txt",
            gmp_copying_v2,
        )
        archive.writestr(
            f"{license_prefix}/GMP-COPYING.LESSERv3.txt",
            b"GNU LESSER GENERAL PUBLIC LICENSE\nVersion 3\n",
        )
        archive.writestr(
            f"{license_prefix}/NTL-copying.txt",
            b"NTL -- A Library for Doing Number Theory\nGNU LGPL version 2.1 or later\n",
        )
        archive.writestr(
            f"{license_prefix}/THIRD-PARTY-NOTICES.txt",
            b"GMP 6.3.0 and NTL 11.6.0 are statically linked.\n"
            b"GMP is conveyed under its GNU GPL version 2 option.\n",
        )
        archive.writestr(
            f"{license_prefix}/SOURCE.txt",
            b"https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz\n"
            b"https://libntl.org/ntl-11.6.0.tar.gz\n",
        )
    (directory / f"{zip_name}.sha256").write_text(
        f"{_sha256(zip_path)}  {zip_name}\n", encoding="utf-8"
    )
    return zip_path


def _make_cli_archive(directory: Path, platform: str) -> Path:
    names = expected_package_names(FIRST_RELEASE_TAG)
    name_by_platform = {
        "linux-x86_64": names[1],
        "macos-arm64": names[2],
        "windows-x86_64": names[3],
    }
    archive_name = name_by_platform[platform]
    root_name = (
        archive_name[: -len(".tar.gz")]
        if archive_name.endswith(".tar.gz")
        else archive_name[: -len(".zip")]
    )
    root = directory / f"tree-{platform}" / root_name
    (root / "bin").mkdir(parents=True)
    (root / "LICENSE").write_bytes(_project_license())
    if platform == "windows-x86_64":
        executable = root / "bin" / "gnfs.exe"
        executable.write_bytes(b"windows test executable")
        dll = root / "bin" / "libgmp-10.dll"
        dll.write_bytes(b"windows test runtime")
        license_path = root / "licenses" / "mingw-w64-ucrt-x86_64-gmp" / "COPYING"
        license_path.parent.mkdir(parents=True)
        license_path.write_text("GNU license fixture\n", encoding="utf-8")
        manifest = {
            "dependencies": [
                {
                    "dll": dll.name,
                    "license_files": [
                        "licenses/mingw-w64-ucrt-x86_64-gmp/COPYING"
                    ],
                    "package": "mingw-w64-ucrt-x86_64-gmp",
                    "package_version": "6.3.0-1",
                    "sha256": _sha256(dll),
                }
            ],
            "runtime": "MSYS2 UCRT64",
            "schema_version": 1,
        }
        (root / "runtime-dependencies.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (root / "README-release.txt").write_text(
            "GNFS v0.1.0 Windows UCRT64 runtime-dependencies.json\n", encoding="utf-8"
        )
        (root / "THIRD_PARTY_NOTICES.txt").write_text(
            "runtime-dependencies.json\nmingw-w64-ucrt-x86_64-gmp 6.3.0-1\n",
            encoding="utf-8",
        )
    else:
        executable = root / "bin" / "gnfs"
        executable.write_bytes(b"posix test executable")
        if platform == "linux-x86_64":
            metadata = {
                "abi_ceilings": LINUX_ABI_CEILINGS,
                "abi_requirements": {
                    "GLIBC": "2.31",
                    "GLIBCXX": "3.4.29",
                    "CXXABI": "1.3.13",
                },
                "architecture": "x86_64",
                "binary_sha256": _sha256(executable),
                "dynamic_dependencies": ["libc.so.6", "libstdc++.so.6"],
                "platform": "linux-x86_64",
                "schema_version": 1,
            }
            (root / "binary-compatibility.json").write_text(
                json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            (root / "README-release.txt").write_text(
                "GLIBC_2.31 GLIBCXX_3.4.29 CXXABI_1.3.13 binary-compatibility.json\n"
                "GMP and NTL dynamic libraries are not bundled.\n",
                encoding="utf-8",
            )
            (root / "THIRD_PARTY_NOTICES.txt").write_text(
                "GLIBC GLIBCXX CXXABI binary-compatibility.json\n"
                "GMP and NTL dynamic libraries are not bundled.\n",
                encoding="utf-8",
            )
        else:
            (root / "README-release.txt").write_text(
                "GMP and NTL dynamic libraries are not bundled.\n", encoding="utf-8"
            )
            (root / "THIRD_PARTY_NOTICES.txt").write_text(
                "GMP and NTL dynamic libraries are not bundled.\n", encoding="utf-8"
            )

    output = directory / archive_name
    if archive_name.endswith(".tar.gz"):
        with tarfile.open(output, mode="w:gz") as archive:
            archive.add(root, arcname=root_name)
    else:
        with zipfile.ZipFile(output, mode="x", compression=zipfile.ZIP_STORED) as archive:
            archive.write(root, arcname=f"{root_name}/")
            for member in sorted(root.rglob("*")):
                relative = member.relative_to(root).as_posix()
                archive.write(
                    member,
                    arcname=f"{root_name}/{relative}{'/' if member.is_dir() else ''}",
                )
    return output


def self_test() -> None:
    target_sha = "1" * 40
    repository = "example/GNFS"
    workflow_ref = f"{repository}/{RELEASE_WORKFLOW_PATH}@refs/heads/main"
    validate_dispatch(
        "verify-only",
        FIRST_RELEASE_TAG,
        target_sha,
        f"VERIFY {FIRST_RELEASE_TAG}",
        repository,
        workflow_ref,
        target_sha,
    )
    validate_dispatch(
        "publish",
        FIRST_RELEASE_TAG,
        target_sha,
        f"PUBLISH {FIRST_RELEASE_TAG}",
        repository,
        workflow_ref,
        target_sha,
    )
    try:
        validate_dispatch(
            "publish",
            FIRST_RELEASE_TAG,
            target_sha,
            f"VERIFY {FIRST_RELEASE_TAG}",
            repository,
            workflow_ref,
            target_sha,
        )
    except ReleaseContractError:
        pass
    else:
        raise ReleaseContractError("publish accepted a verify-only confirmation")

    client = _FakeClient(target_sha)
    evidence = verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
    if evidence["workbench_run_id"] <= 0:
        raise ReleaseContractError("main CI self-test did not resolve Workbench evidence")
    client.checks[0]["conclusion"] = "failure"
    try:
        verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
    except ReleaseContractError:
        pass
    else:
        raise ReleaseContractError("main CI self-test accepted a failed required check")
    client.checks[0]["conclusion"] = "success"

    external_check = next(
        check for check in client.checks if check.get("id") == client.external_check_id
    )

    def expect_external_rejection(label: str) -> None:
        try:
            verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
        except ReleaseContractError:
            return
        raise ReleaseContractError(f"main CI self-test accepted {label} external CodeQL")

    external_check["status"] = "in_progress"
    external_check["conclusion"] = None
    expect_external_rejection("pending")
    external_check["status"] = "completed"
    external_check["conclusion"] = "failure"
    expect_external_rejection("failed")
    external_check["conclusion"] = "success"
    external_check["app"] = {"id": 1, "slug": "wrong-app", "name": "Wrong App"}
    expect_external_rejection("wrong-app")
    external_check["app"] = {
        "id": 57789,
        "slug": "github-advanced-security",
        "name": "GitHub Advanced Security",
    }
    external_index = client.checks.index(external_check)
    client.checks.pop(external_index)
    expect_external_rejection("missing")
    client.checks.insert(external_index, external_check)
    duplicate_external = {
        **external_check,
        "id": client.external_check_id + 1,
        "app": dict(external_check["app"]),
    }
    client.checks.append(duplicate_external)
    expect_external_rejection("duplicate")
    client.checks.pop()

    proof_run_id = 3000
    proof_name = _artifact_name("release-verification", FIRST_RELEASE_TAG, target_sha)
    assets_name = _artifact_name("release-assets", FIRST_RELEASE_TAG, target_sha)
    client.proof_artifacts = [
        {
            "id": 99,
            "name": proof_name,
            "expired": False,
            "workflow_run": {"id": proof_run_id, "head_sha": target_sha},
        }
    ]
    client.run_details[proof_run_id] = {
        "name": "Release Artifacts",
        "path": RELEASE_WORKFLOW_PATH,
        "event": "workflow_dispatch",
        "head_branch": "main",
        "head_sha": target_sha,
        "status": "completed",
        "conclusion": "success",
    }
    client.run_artifacts[proof_run_id] = [
        {"id": 100, "name": assets_name, "expired": False}
    ]
    if find_verification_run(client, repository, target_sha, FIRST_RELEASE_TAG) != proof_run_id:
        raise ReleaseContractError("verification-run self-test selected the wrong run")

    with tempfile.TemporaryDirectory(prefix="gnfs-release-contract-self-test-") as temp_dir:
        root = Path(temp_dir)
        workbench = root / "workbench"
        workbench.mkdir()
        workbench_zip = _make_workbench_artifact(workbench, target_sha)
        validate_workbench_artifact(workbench, target_sha, FIRST_RELEASE_TAG)

        gmp_resource_suffix = (
            "GNFSWorkbench.app/Contents/Resources/Licenses/GMP-COPYING.txt"
        )
        with zipfile.ZipFile(workbench_zip) as valid_workbench_archive:
            gmp_copying_v2 = valid_workbench_archive.read(gmp_resource_suffix)

        def expect_workbench_license_rejection(
            label: str, replacement: bytes, expected_error: str
        ) -> None:
            tampered_directory = root / f"bad-workbench-{label}"
            tampered_directory.mkdir()
            tampered_zip = tampered_directory / workbench_zip.name
            with zipfile.ZipFile(workbench_zip) as source_archive, zipfile.ZipFile(
                tampered_zip, mode="x", compression=zipfile.ZIP_STORED
            ) as destination_archive:
                for entry in source_archive.infolist():
                    content = source_archive.read(entry.filename)
                    if entry.filename == gmp_resource_suffix:
                        content = replacement
                    destination_archive.writestr(entry, content)
            (tampered_directory / f"{workbench_zip.name}.sha256").write_text(
                f"{_sha256(tampered_zip)}  {workbench_zip.name}\n", encoding="utf-8"
            )
            try:
                validate_workbench_artifact(
                    tampered_directory, target_sha, FIRST_RELEASE_TAG
                )
            except ReleaseContractError as error:
                if expected_error not in str(error):
                    raise ReleaseContractError(
                        f"Workbench {label} self-test failed for the wrong reason: {error}"
                    ) from error
            else:
                raise ReleaseContractError(
                    f"Workbench validation accepted {label} GMP COPYING text"
                )

        expect_workbench_license_rejection(
            "altered",
            gmp_copying_v2 + b"\n",
            "not the pinned upstream text",
        )
        expect_workbench_license_rejection(
            "GPLv3",
            b"GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n",
            "forbidden identity markers",
        )

        assets = root / "assets"
        assets.mkdir()
        cli_fixtures = root / "cli-fixtures"
        cli_fixtures.mkdir()
        for platform in ("linux-x86_64", "macos-arm64", "windows-x86_64"):
            archive_path = _make_cli_archive(cli_fixtures, platform)
            packaged = assets / archive_path.name
            packaged.write_bytes(archive_path.read_bytes())
            validate_cli_archive(packaged, platform, FIRST_RELEASE_TAG)
        linux_root = "gnfs-v0.1.0-linux-x86_64"
        linux_binary = b"linux ABI negative fixture"
        too_new_metadata = {
            "abi_ceilings": LINUX_ABI_CEILINGS,
            "abi_requirements": {
                "GLIBC": "2.32",
                "GLIBCXX": "3.4.30",
                "CXXABI": "1.3.13",
            },
            "architecture": "x86_64",
            "binary_sha256": hashlib.sha256(linux_binary).hexdigest(),
            "dynamic_dependencies": ["libc.so.6", "libstdc++.so.6"],
            "platform": "linux-x86_64",
            "schema_version": 1,
        }
        negative_files = {
            f"{linux_root}/bin/gnfs": linux_binary,
            f"{linux_root}/binary-compatibility.json": (
                json.dumps(too_new_metadata, sort_keys=True) + "\n"
            ).encode(),
        }
        try:
            _validate_linux_binary_metadata(
                linux_root,
                set(negative_files),
                negative_files.__getitem__,
                "GLIBC_2.32 GLIBCXX_3.4.30 CXXABI_1.3.13 binary-compatibility.json",
                "GLIBC GLIBCXX CXXABI binary-compatibility.json",
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("Linux archive contract accepted an ABI above its ceiling")
        bad_cli = root / "bad-cli"
        bad_cli.mkdir()
        windows_name = expected_package_names(FIRST_RELEASE_TAG)[3]
        with zipfile.ZipFile(assets / windows_name) as source_archive, zipfile.ZipFile(
            bad_cli / windows_name, mode="x", compression=zipfile.ZIP_STORED
        ) as bad_archive:
            for entry in source_archive.infolist():
                if entry.filename.endswith("/LICENSE"):
                    continue
                bad_archive.writestr(entry, source_archive.read(entry.filename))
        try:
            validate_cli_archive(
                bad_cli / windows_name, "windows-x86_64", FIRST_RELEASE_TAG
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("CLI archive validation accepted a missing project LICENSE")
        (assets / workbench_zip.name).write_bytes(workbench_zip.read_bytes())
        assemble_release_bundle(assets, target_sha, FIRST_RELEASE_TAG, 1_700_000_000)
        verify_release_bundle(assets, target_sha, FIRST_RELEASE_TAG)
        proof = root / "release-verification.json"
        ci_evidence_path = root / "main-ci-evidence.json"
        ci_evidence_path.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        write_verification_proof(
            assets, proof, ci_evidence_path, target_sha, FIRST_RELEASE_TAG
        )
        verify_verification_proof(proof, assets, target_sha, FIRST_RELEASE_TAG)
        tampered_proof_payload = _read_json_object(proof)
        tampered_proof_payload["main_ci_evidence"]["required_external_checks"][0][
            "app_id"
        ] = 1
        tampered_proof = root / "tampered-release-verification.json"
        tampered_proof.write_text(
            json.dumps(tampered_proof_payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        try:
            verify_verification_proof(
                tampered_proof, assets, target_sha, FIRST_RELEASE_TAG
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("verification proof accepted altered external CodeQL evidence")

        release_id = 7000
        client.tag_ref = {
            "ref": f"refs/tags/{FIRST_RELEASE_TAG}",
            "object": {"type": "commit", "sha": target_sha},
        }
        release_asset_names = sorted(
            (*expected_package_names(FIRST_RELEASE_TAG), "release-metadata.json", "SHA256SUMS")
        )
        client.release = {
            "id": release_id,
            "draft": True,
            "prerelease": False,
            "tag_name": FIRST_RELEASE_TAG,
            "target_commitish": target_sha,
            "assets": [
                {
                    "name": name,
                    "state": "uploaded",
                    "size": (assets / name).stat().st_size,
                    "digest": f"sha256:{_sha256(assets / name)}",
                }
                for name in release_asset_names
            ],
        }
        result = final_prepublish(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            release_id,
            assets,
        )
        if result["asset_count"] != len(release_asset_names):
            raise ReleaseContractError("final prepublish self-test lost release assets")

        client.checks[0]["conclusion"] = "failure"
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted failed exact-SHA CI")
        client.checks[0]["conclusion"] = "success"

        original_main = client.target_sha
        client.target_sha = "2" * 40
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted a moved main branch")
        client.target_sha = original_main

        original_tag_sha = client.tag_ref["object"]["sha"]
        client.tag_ref["object"]["sha"] = "3" * 40
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted a moved release tag")
        client.tag_ref["object"]["sha"] = original_tag_sha

        client.release["id"] = release_id + 1
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted a different draft release")
        client.release["id"] = release_id

        client.release["assets"][0]["digest"] = f"sha256:{'4' * 64}"
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted a changed draft asset")
        client.release["assets"][0]["digest"] = (
            f"sha256:{_sha256(assets / client.release['assets'][0]['name'])}"
        )

        client.release["draft"] = False
        try:
            final_prepublish(
                client, repository, target_sha, FIRST_RELEASE_TAG, release_id, assets
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("final prepublish accepted a public release")
        client.release["draft"] = True

        cli_asset = assets / expected_package_names(FIRST_RELEASE_TAG)[1]
        original = cli_asset.read_bytes()
        cli_asset.write_bytes(original + b"tampered")
        try:
            verify_verification_proof(proof, assets, target_sha, FIRST_RELEASE_TAG)
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("verification proof accepted a modified release asset")

    repository_root = Path(__file__).resolve().parents[1]
    release_workflow = repository_root / ".github/workflows/release.yml"
    qualification_workflow = repository_root / ".github/workflows/release-qualification.yml"
    validate_workflow_sources(release_workflow, qualification_workflow)


def _add_identity_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--release-tag", default=FIRST_RELEASE_TAG)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    dispatch = subparsers.add_parser("validate-dispatch")
    dispatch.add_argument("--mode", required=True)
    _add_identity_arguments(dispatch)
    dispatch.add_argument("--confirmation", required=True)
    dispatch.add_argument("--repository", required=True)
    dispatch.add_argument("--workflow-ref", required=True)
    dispatch.add_argument("--workflow-sha", required=True)

    checkout = subparsers.add_parser("verify-checkout")
    checkout.add_argument("--target-sha", required=True)
    checkout.add_argument("--repository-root", type=Path, default=Path.cwd())

    verify_main = subparsers.add_parser("verify-main")
    _add_identity_arguments(verify_main)
    verify_main.add_argument("--repository", required=True)
    verify_main.add_argument("--github-output", type=Path)
    verify_main.add_argument("--evidence-output", type=Path)

    find_proof = subparsers.add_parser("find-verification")
    _add_identity_arguments(find_proof)
    find_proof.add_argument("--repository", required=True)
    find_proof.add_argument("--github-output", type=Path)

    workbench = subparsers.add_parser("validate-workbench")
    _add_identity_arguments(workbench)
    workbench.add_argument("--artifact-directory", type=Path, required=True)

    cli_archive = subparsers.add_parser("validate-cli-archive")
    cli_archive.add_argument("--archive", type=Path, required=True)
    cli_archive.add_argument(
        "--platform",
        choices=("linux-x86_64", "macos-arm64", "windows-x86_64"),
        required=True,
    )
    cli_archive.add_argument("--release-tag", default=FIRST_RELEASE_TAG)

    assemble = subparsers.add_parser("assemble")
    _add_identity_arguments(assemble)
    assemble.add_argument("--asset-directory", type=Path, required=True)
    assemble.add_argument("--source-date-epoch", type=int, required=True)

    verify_bundle = subparsers.add_parser("verify-bundle")
    _add_identity_arguments(verify_bundle)
    verify_bundle.add_argument("--asset-directory", type=Path, required=True)

    proof = subparsers.add_parser("write-proof")
    _add_identity_arguments(proof)
    proof.add_argument("--asset-directory", type=Path, required=True)
    proof.add_argument("--output", type=Path, required=True)
    proof.add_argument("--ci-evidence", type=Path, required=True)

    verify_proof = subparsers.add_parser("verify-proof")
    _add_identity_arguments(verify_proof)
    verify_proof.add_argument("--asset-directory", type=Path, required=True)
    verify_proof.add_argument("--proof", type=Path, required=True)

    prepublish = subparsers.add_parser("final-prepublish")
    _add_identity_arguments(prepublish)
    prepublish.add_argument("--repository", required=True)
    prepublish.add_argument("--expected-release-id", type=int, required=True)
    prepublish.add_argument("--asset-directory", type=Path, required=True)

    workflows = subparsers.add_parser("check-workflows")
    workflows.add_argument(
        "--release-workflow",
        type=Path,
        default=Path(".github/workflows/release.yml"),
    )
    workflows.add_argument(
        "--qualification-workflow",
        type=Path,
        default=Path(".github/workflows/release-qualification.yml"),
    )

    subparsers.add_parser("self-test")
    return parser.parse_args()


def _client_from_environment() -> GitHubClient:
    return GitHubClient(
        token=os.environ.get("GITHUB_TOKEN", ""),
        api_url=os.environ.get("GITHUB_API_URL", "https://api.github.com"),
    )


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "validate-dispatch":
            validate_dispatch(
                arguments.mode,
                arguments.release_tag,
                arguments.target_sha,
                arguments.confirmation,
                arguments.repository,
                arguments.workflow_ref,
                arguments.workflow_sha,
            )
        elif arguments.command == "verify-checkout":
            verify_checkout(arguments.target_sha, arguments.repository_root)
        elif arguments.command == "verify-main":
            evidence = verify_main_ci(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
            )
            print(json.dumps(evidence, indent=2, sort_keys=True))
            if arguments.evidence_output is not None:
                evidence_output = arguments.evidence_output.resolve()
                if evidence_output.exists() or evidence_output.is_symlink():
                    raise ReleaseContractError(
                        f"refusing to overwrite main CI evidence: {evidence_output}"
                    )
                evidence_output.parent.mkdir(parents=True, exist_ok=True)
                with evidence_output.open("x", encoding="utf-8", newline="\n") as handle:
                    json.dump(evidence, handle, indent=2, sort_keys=True)
                    handle.write("\n")
            _write_github_output(
                arguments.github_output, "workbench_run_id", evidence["workbench_run_id"]
            )
        elif arguments.command == "find-verification":
            run_id = find_verification_run(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
            )
            print(run_id)
            _write_github_output(arguments.github_output, "verification_run_id", run_id)
        elif arguments.command == "validate-workbench":
            validate_workbench_artifact(
                arguments.artifact_directory, arguments.target_sha, arguments.release_tag
            )
        elif arguments.command == "validate-cli-archive":
            validate_cli_archive(
                arguments.archive, arguments.platform, arguments.release_tag
            )
        elif arguments.command == "assemble":
            assemble_release_bundle(
                arguments.asset_directory,
                arguments.target_sha,
                arguments.release_tag,
                arguments.source_date_epoch,
            )
        elif arguments.command == "verify-bundle":
            verify_release_bundle(
                arguments.asset_directory, arguments.target_sha, arguments.release_tag
            )
        elif arguments.command == "write-proof":
            write_verification_proof(
                arguments.asset_directory,
                arguments.output,
                arguments.ci_evidence,
                arguments.target_sha,
                arguments.release_tag,
            )
        elif arguments.command == "verify-proof":
            verify_verification_proof(
                arguments.proof,
                arguments.asset_directory,
                arguments.target_sha,
                arguments.release_tag,
            )
        elif arguments.command == "final-prepublish":
            evidence = final_prepublish(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
                arguments.expected_release_id,
                arguments.asset_directory,
            )
            print(json.dumps(evidence, indent=2, sort_keys=True))
        elif arguments.command == "check-workflows":
            validate_workflow_sources(
                arguments.release_workflow, arguments.qualification_workflow
            )
            print("release workflow source contract: PASS")
        elif arguments.command == "self-test":
            self_test()
            print("release contract self-test: PASS")
    except (
        ReleaseContractError,
        OSError,
        subprocess.CalledProcessError,
        zipfile.BadZipFile,
    ) as error:
        print(f"release contract error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
