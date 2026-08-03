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
import subprocess
import sys
import tempfile
from typing import Any, Protocol
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


@dataclass(frozen=True)
class RequiredCheck:
    workflow: str
    workflow_path: str
    job: str


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


def verify_main_ci(
    client: GitHubAPI, repository: str, target_sha: str, release_tag: str
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
        f"/repos/{repository}/commits/{target_sha}/check-runs", "check_runs"
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
    return {
        "schema_version": 1,
        "target_sha": target_sha,
        "all_triggered_push_workflows": len(exact_runs),
        "required_checks": evidence,
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
    version = release_tag.removeprefix("v")
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
    except (zipfile.BadZipFile, KeyError, ValueError, plistlib.InvalidFileException) as error:
        raise ReleaseContractError(f"invalid Workbench ZIP: {error}") from error
    if info.get(WORKBENCH_INFO_KEY) != target_sha:
        raise ReleaseContractError(
            f"Workbench {WORKBENCH_INFO_KEY} does not match target SHA"
        )
    if info.get("CFBundleShortVersionString") != release_tag.removeprefix("v"):
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


def write_verification_proof(
    asset_directory: Path,
    output: Path,
    target_sha: str,
    release_tag: str,
) -> None:
    verify_release_bundle(asset_directory, target_sha, release_tag)
    output = output.resolve()
    if output.exists() or output.is_symlink():
        raise ReleaseContractError(f"refusing to overwrite verification proof: {output}")
    bundle_names = sorted(
        (*expected_package_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    proof = {
        "schema_version": 1,
        "mode": "verify-only",
        "release_tag": release_tag,
        "target_sha": target_sha,
        "required_main_checks": _required_check_contract(),
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
        "bundle",
    }
    if set(proof) != expected_keys:
        raise ReleaseContractError("verification proof contains missing or unknown fields")
    if (
        proof.get("schema_version") != 1
        or proof.get("mode") != "verify-only"
        or proof.get("release_tag") != release_tag
        or proof.get("target_sha") != target_sha
        or proof.get("required_main_checks") != _required_check_contract()
    ):
        raise ReleaseContractError("verification proof identity or required checks changed")
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
        "release-verification-${{ inputs.release_tag }}-${{ inputs.target_sha }}",
        "GNFSWorkbench-0.1.0-macOS-arm64.zip",
        "--draft",
        "--draft=false",
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
        self.proof_artifacts: list[dict[str, Any]] = []
        self.run_details: dict[int, dict[str, Any]] = {}
        self.run_artifacts: dict[int, list[dict[str, Any]]] = {}

    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        del query
        if path.endswith("/git/ref/heads/main"):
            return {"object": {"sha": self.target_sha}}
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
    info = {
        "CFBundleShortVersionString": FIRST_RELEASE_TAG.removeprefix("v"),
        WORKBENCH_INFO_KEY: target_sha,
    }
    with zipfile.ZipFile(zip_path, mode="x") as archive:
        archive.writestr(
            "GNFSWorkbench.app/Contents/Info.plist",
            plistlib.dumps(info, fmt=plistlib.FMT_BINARY),
        )
        archive.writestr("GNFSWorkbench.app/Contents/MacOS/GNFSWorkbench", b"binary")
    (directory / f"{zip_name}.sha256").write_text(
        f"{_sha256(zip_path)}  {zip_name}\n", encoding="utf-8"
    )
    return zip_path


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

        assets = root / "assets"
        assets.mkdir()
        for name in expected_package_names(FIRST_RELEASE_TAG)[1:]:
            (assets / name).write_bytes(f"test payload for {name}\n".encode())
        (assets / workbench_zip.name).write_bytes(workbench_zip.read_bytes())
        assemble_release_bundle(assets, target_sha, FIRST_RELEASE_TAG, 1_700_000_000)
        verify_release_bundle(assets, target_sha, FIRST_RELEASE_TAG)
        proof = root / "release-verification.json"
        write_verification_proof(assets, proof, target_sha, FIRST_RELEASE_TAG)
        verify_verification_proof(proof, assets, target_sha, FIRST_RELEASE_TAG)

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

    find_proof = subparsers.add_parser("find-verification")
    _add_identity_arguments(find_proof)
    find_proof.add_argument("--repository", required=True)
    find_proof.add_argument("--github-output", type=Path)

    workbench = subparsers.add_parser("validate-workbench")
    _add_identity_arguments(workbench)
    workbench.add_argument("--artifact-directory", type=Path, required=True)

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

    verify_proof = subparsers.add_parser("verify-proof")
    _add_identity_arguments(verify_proof)
    verify_proof.add_argument("--asset-directory", type=Path, required=True)
    verify_proof.add_argument("--proof", type=Path, required=True)

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
