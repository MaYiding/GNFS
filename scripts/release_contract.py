#!/usr/bin/env python3
"""Validate GNFS release inputs, CI evidence, artifacts, and publication proofs."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
import hashlib
from http.client import IncompleteRead
import io
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
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen
import zipfile

from windows_runtime_contract import (
    CONTRACT_PATH as WINDOWS_RUNTIME_CONTRACT_PATH,
    WindowsRuntimeContractError,
    load_contract as load_windows_runtime_contract,
)


FIRST_RELEASE_TAG = "v0.1.0"
GITHUB_API_VERSION = "2026-03-10"
RELEASE_PROOF_NAME = "release-verification.json"
RELEASE_TAG_RULESET_ID = 20335185
RELEASE_TAG_RULESET_NAME = "Protect release tags"
RELEASE_TAG_RULESET_NODE_ID = "RRS_lACqUmVwb3NpdG9yec5GF9b-zgE2SlE"
RELEASE_TAG_RULESET_CREATED_AT = "2026-08-03T22:46:28.399Z"
RELEASE_TAG_RULESET_UPDATED_AT = "2026-08-03T22:46:28.413Z"
RELEASE_STATE_EMPTY = "unpublished-empty"
RELEASE_STATE_DRAFT = "resumable-draft"
RELEASE_STATE_PUBLISHED = "published-immutable"
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
DEPENDENCY_SOURCE_URLS = {
    "gmp-6.3.0.tar.xz": (
        "https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz",
        "https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz",
    ),
    "ntl-11.6.0.tar.gz": ("https://libntl.org/ntl-11.6.0.tar.gz",),
}
DEPENDENCY_SOURCE_SHA256 = {
    "gmp-6.3.0.tar.xz": "a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898",
    "ntl-11.6.0.tar.gz": "bc0ef9aceb075a6a0673ac8d8f47d5f8458c72fe806e4468fbd5d3daff056182",
}
DEPENDENCY_SOURCE_ROOTS = {
    "gmp-6.3.0.tar.xz": "gmp-6.3.0",
    "ntl-11.6.0.tar.gz": "ntl-11.6.0",
}
MAX_DEPENDENCY_SOURCE_BYTES = 192 * 1024 * 1024


@dataclass(frozen=True)
class RequiredCheck:
    workflow: str
    workflow_path: str
    job: str


@dataclass(frozen=True)
class RequiredCodeScanningAnalysis:
    tool_name: str
    analysis_key: str
    category: str
    environment: str


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
    RequiredCheck(
        "Release Readiness",
        ".github/workflows/release-readiness.yml",
        "Windows pinned runtime and source closure",
    ),
)
REQUIRED_CODE_SCANNING_ANALYSES = (
    RequiredCodeScanningAnalysis(
        "CodeQL",
        ".github/workflows/codeql.yml:analyze",
        ".github/workflows/codeql.yml:analyze",
        "{}",
    ),
)


class ReleaseContractError(RuntimeError):
    """Raised when release evidence violates a fail-closed contract."""


class GitHubAPIRequestError(ReleaseContractError):
    """Raised when GitHub rejects an API request, retaining the HTTP status."""

    def __init__(self, status: int, path: str, detail: str) -> None:
        super().__init__(f"GitHub API {status} for {path}: {detail}")
        self.status = status
        self.path = path


def _windows_runtime_contract():
    try:
        return load_windows_runtime_contract()
    except WindowsRuntimeContractError as error:
        raise ReleaseContractError(f"Windows runtime source contract is invalid: {error}") from error


def _dependency_source_contracts() -> tuple[
    dict[str, tuple[str, ...]], dict[str, str], dict[str, str]
]:
    urls = dict(DEPENDENCY_SOURCE_URLS)
    digests = dict(DEPENDENCY_SOURCE_SHA256)
    roots = dict(DEPENDENCY_SOURCE_ROOTS)
    for source in _windows_runtime_contract().source_archives:
        if source.name in urls or source.name in digests or source.name in roots:
            raise ReleaseContractError(f"duplicate dependency source contract: {source.name}")
        urls[source.name] = (source.url,)
        digests[source.name] = source.sha256
        roots[source.name] = source.root
    return urls, digests, roots


class GitHubAPI(Protocol):
    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        """Return one GitHub API response."""

    def get_optional(self, path: str) -> Any | None:
        """Return one response or None for HTTP 404."""

    def patch(self, path: str, payload: dict[str, Any]) -> Any:
        """PATCH one resource and return its response."""

    def post(self, path: str, payload: dict[str, Any]) -> Any:
        """POST one resource and return its response."""

    def upload_release_asset(
        self, repository: str, release_id: int, name: str, path: Path
    ) -> Any:
        """Upload one release asset without replacing existing state."""

    def paginate(self, path: str, key: str, query: dict[str, str] | None = None) -> list[Any]:
        """Return every item from a paginated GitHub API response."""

    def paginate_array(
        self, path: str, query: dict[str, str] | None = None
    ) -> list[Any]:
        """Return every item from an array-valued paginated response."""


class GitHubClient:
    def __init__(self, token: str, api_url: str = "https://api.github.com") -> None:
        if not token:
            raise ReleaseContractError("GITHUB_TOKEN is required for release verification")
        self._token = token
        self._api_url = api_url.rstrip("/")
        self._ssl_context = _verified_ssl_context()

    def _request(
        self,
        path: str,
        *,
        method: str = "GET",
        query: dict[str, str] | None = None,
        payload: dict[str, Any] | None = None,
    ) -> Any:
        suffix = path if path.startswith("/") else f"/{path}"
        url = f"{self._api_url}{suffix}"
        if query:
            url = f"{url}?{urlencode(query)}"
        data = None
        if payload is not None:
            data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        request = Request(
            url,
            data=data,
            method=method,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "Content-Type": "application/json",
                "X-GitHub-Api-Version": GITHUB_API_VERSION,
                "User-Agent": "gnfs-release-contract/1",
            },
        )
        try:
            with urlopen(request, timeout=30, context=self._ssl_context) as response:
                return json.load(response)
        except HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")[:1000]
            raise GitHubAPIRequestError(error.code, path, detail) from error
        except (URLError, TimeoutError, json.JSONDecodeError) as error:
            raise ReleaseContractError(f"GitHub API request failed for {path}: {error}") from error

    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        return self._request(path, query=query)

    def patch(self, path: str, payload: dict[str, Any]) -> Any:
        return self._request(path, method="PATCH", payload=payload)

    def post(self, path: str, payload: dict[str, Any]) -> Any:
        return self._request(path, method="POST", payload=payload)

    def upload_release_asset(
        self, repository: str, release_id: int, name: str, path: Path
    ) -> Any:
        _validate_repository(repository)
        if release_id <= 0 or not re.fullmatch(r"[A-Za-z0-9._-]+", name):
            raise ReleaseContractError("release asset upload identity is invalid")
        if path.is_symlink() or not path.is_file():
            raise ReleaseContractError(f"release asset is not a real file: {name}")
        if self._api_url != "https://api.github.com":
            raise ReleaseContractError("release asset upload currently requires GitHub.com")
        url = (
            f"https://uploads.github.com/repos/{repository}/releases/{release_id}/assets?"
            + urlencode({"name": name})
        )
        request = Request(
            url,
            data=path.read_bytes(),
            method="POST",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "Content-Type": "application/octet-stream",
                "X-GitHub-Api-Version": GITHUB_API_VERSION,
                "User-Agent": "gnfs-release-contract/1",
            },
        )
        try:
            with urlopen(request, timeout=300, context=self._ssl_context) as response:
                return json.load(response)
        except HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")[:1000]
            raise GitHubAPIRequestError(error.code, url, detail) from error
        except (URLError, TimeoutError, json.JSONDecodeError) as error:
            raise ReleaseContractError(
                f"GitHub release asset upload failed for {name}: {error}"
            ) from error

    def get_optional(self, path: str) -> Any | None:
        suffix = path if path.startswith("/") else f"/{path}"
        url = f"{self._api_url}{suffix}"
        request = Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "X-GitHub-Api-Version": GITHUB_API_VERSION,
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
            raise GitHubAPIRequestError(error.code, path, detail) from error
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

    def paginate_array(
        self, path: str, query: dict[str, str] | None = None
    ) -> list[Any]:
        items: list[Any] = []
        page = 1
        while True:
            page_query = dict(query or {})
            page_query.update({"per_page": "100", "page": str(page)})
            page_items = self.get(path, page_query)
            if not isinstance(page_items, list):
                raise ReleaseContractError(
                    f"GitHub API response is not an array for {path}"
                )
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


def _target_commit_epoch(target_sha: str) -> int:
    """Return the exact committer epoch for a target commit in this repository."""

    _validate_sha(target_sha)
    repository_root = Path(__file__).resolve().parents[1]
    try:
        result = subprocess.run(
            ["git", "show", "-s", "--format=%ct", target_sha],
            cwd=repository_root,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        raise ReleaseContractError(
            f"unable to resolve target commit timestamp: {error.stderr.strip()[:1000]}"
        ) from error
    value = result.stdout.strip()
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise ReleaseContractError("target commit timestamp is not a positive Unix epoch")
    return int(value)


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


def _release_candidates(
    client: GitHubAPI, repository: str, release_tag: str
) -> list[dict[str, Any]]:
    releases = client.paginate_array(f"/repos/{repository}/releases")
    if any(not isinstance(release, dict) for release in releases):
        raise ReleaseContractError("repository release list contains an invalid record")
    return [release for release in releases if release.get("tag_name") == release_tag]


def _verify_resumable_draft_identity(
    release: dict[str, Any], target_sha: str, release_tag: str
) -> int:
    release_id = release.get("id")
    if (
        not isinstance(release_id, int)
        or release_id <= 0
        or release.get("tag_name") != release_tag
        or release.get("target_commitish") != target_sha
        or release.get("draft") is not True
        or release.get("prerelease") is not False
        or release.get("immutable") is not False
    ):
        raise ReleaseContractError("existing release is not the exact resumable draft")
    return release_id


def _verify_release_state(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    *,
    allow_exact_published: bool,
) -> tuple[str, int | None]:
    """Classify the tag as empty, one exact draft, or one exact public release."""

    encoded_tag = quote(release_tag, safe="")
    tag_ref = client.get_optional(f"/repos/{repository}/git/ref/tags/{encoded_tag}")
    candidates = _release_candidates(client, repository, release_tag)
    if len(candidates) > 1:
        raise ReleaseContractError(f"multiple releases claim tag {release_tag}")
    if tag_ref is None:
        if not candidates:
            return RELEASE_STATE_EMPTY, None
        release_id = _verify_resumable_draft_identity(
            candidates[0], target_sha, release_tag
        )
        _verify_prepared_draft(candidates[0], release_id, target_sha, release_tag)
        return RELEASE_STATE_DRAFT, release_id

    if not allow_exact_published:
        raise ReleaseContractError(f"refusing to reuse existing tag {release_tag}")
    if len(candidates) != 1:
        raise ReleaseContractError(
            "published release recovery requires one exact release candidate"
        )
    release_id = candidates[0].get("id")
    if type(release_id) is not int or release_id <= 0:
        raise ReleaseContractError("published release has no positive numeric id")
    public_by_id = client.get(f"/repos/{repository}/releases/{release_id}")
    public_by_tag = client.get(f"/repos/{repository}/releases/tags/{encoded_tag}")
    for public_release in (candidates[0], public_by_id, public_by_tag):
        _verify_release_identity(
            public_release,
            expected_release_id=release_id,
            release_tag=release_tag,
            target_sha=target_sha,
            draft=False,
            immutable=True,
        )
    _verify_release_tag_record(tag_ref, release_tag, target_sha)
    return RELEASE_STATE_PUBLISHED, release_id


def verify_repository_protection(
    client: GitHubAPI,
    repository: str,
    *,
    allow_unreadable_immutable_setting: bool,
) -> dict[str, Any]:
    """Verify the repository-owned tag boundary and immutable-release setting.

    GitHub's immutable-release setting endpoint requires Administration (read),
    which the ephemeral Actions token cannot be granted.  The caller may allow
    only that documented visibility gap; publication still requires the public
    release object itself to report ``immutable: true``.
    """

    _validate_repository(repository)
    ruleset = client.get(f"/repos/{repository}/rulesets/{RELEASE_TAG_RULESET_ID}")

    def utc_timestamp(value: Any) -> str:
        if not isinstance(value, str):
            raise ReleaseContractError("release tag ruleset timestamp is missing")
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as error:
            raise ReleaseContractError("release tag ruleset timestamp is invalid") from error
        if parsed.tzinfo is None:
            raise ReleaseContractError("release tag ruleset timestamp lacks a timezone")
        return (
            parsed.astimezone(timezone.utc)
            .isoformat(timespec="milliseconds")
            .replace("+00:00", "Z")
        )

    expected_ruleset = {
        "id": RELEASE_TAG_RULESET_ID,
        "node_id": RELEASE_TAG_RULESET_NODE_ID,
        "name": RELEASE_TAG_RULESET_NAME,
        "target": "tag",
        "source_type": "Repository",
        "source": repository,
        "enforcement": "active",
        "current_user_can_bypass": "never",
        "conditions": {
            "ref_name": {"include": ["refs/tags/v*"], "exclude": []},
        },
        "rules": [{"type": "update"}, {"type": "deletion"}],
    }
    observed_ruleset = {key: ruleset.get(key) for key in expected_ruleset}
    if observed_ruleset != expected_ruleset:
        raise ReleaseContractError("release tag protection ruleset changed from the exact contract")
    if (
        utc_timestamp(ruleset.get("created_at")) != RELEASE_TAG_RULESET_CREATED_AT
        or utc_timestamp(ruleset.get("updated_at")) != RELEASE_TAG_RULESET_UPDATED_AT
    ):
        raise ReleaseContractError("release tag protection ruleset version changed")
    # GitHub omits bypass_actors unless the caller has ruleset write access.
    # The pinned node/version detects hidden edits; if the field is visible it
    # must independently confirm the empty-bypass contract.
    if "bypass_actors" in ruleset and ruleset["bypass_actors"] != []:
        raise ReleaseContractError("release tag protection ruleset has a bypass actor")

    immutable_setting: dict[str, Any] | None = None
    try:
        payload = client.get(f"/repos/{repository}/immutable-releases")
        if payload != {"enabled": True, "enforced_by_owner": False}:
            raise ReleaseContractError(
                "repository immutable-release setting is not the expected enabled repository policy"
            )
        immutable_setting = payload
    except GitHubAPIRequestError as error:
        if not allow_unreadable_immutable_setting or error.status != 403:
            raise

    return {
        "schema_version": 1,
        "tag_ruleset_id": RELEASE_TAG_RULESET_ID,
        "tag_ruleset": "exact",
        "immutable_setting": (
            "enabled" if immutable_setting is not None else "administration-read-unavailable"
        ),
    }


def _verify_required_code_scanning_analyses(
    client: GitHubAPI, repository: str, target_sha: str
) -> list[dict[str, Any]]:
    analyses = client.paginate_array(
        f"/repos/{repository}/code-scanning/analyses",
        {"ref": "refs/heads/main"},
    )
    evidence: list[dict[str, Any]] = []
    for required in REQUIRED_CODE_SCANNING_ANALYSES:
        stream = [
            analysis
            for analysis in analyses
            if isinstance(analysis, dict)
            and analysis.get("ref") == "refs/heads/main"
            and analysis.get("analysis_key") == required.analysis_key
            and analysis.get("category") == required.category
            and analysis.get("environment") == required.environment
            and (analysis.get("tool") or {}).get("name") == required.tool_name
        ]
        if not stream:
            raise ReleaseContractError(
                f"no exact-main {required.tool_name!r} code-scanning analysis stream exists"
            )

        ordered: list[tuple[datetime, int, dict[str, Any]]] = []
        seen_order_keys: set[tuple[datetime, int]] = set()
        for analysis in stream:
            analysis_id = analysis.get("id")
            created_at = analysis.get("created_at")
            if not isinstance(analysis_id, int) or analysis_id <= 0:
                raise ReleaseContractError(
                    f"{required.tool_name} code-scanning analysis has no positive numeric id"
                )
            if not isinstance(created_at, str):
                raise ReleaseContractError(
                    f"{required.tool_name} code-scanning analysis lacks created_at"
                )
            try:
                created = datetime.fromisoformat(created_at.replace("Z", "+00:00"))
            except ValueError as error:
                raise ReleaseContractError(
                    f"{required.tool_name} code-scanning analysis has invalid created_at"
                ) from error
            if created.tzinfo is None:
                raise ReleaseContractError(
                    f"{required.tool_name} code-scanning analysis timestamp lacks timezone"
                )
            created = created.astimezone(timezone.utc)
            order_key = (created, analysis_id)
            if order_key in seen_order_keys:
                raise ReleaseContractError(
                    f"{required.tool_name} code-scanning analysis ordering is ambiguous"
                )
            seen_order_keys.add(order_key)
            ordered.append((created, analysis_id, analysis))

        created, analysis_id, analysis = max(ordered, key=lambda item: (item[0], item[1]))
        analysis_id = analysis.get("id")
        tool = analysis.get("tool") or {}
        tool_version = tool.get("version")
        results_count = analysis.get("results_count")
        if (
            analysis.get("commit_sha") != target_sha
            or analysis.get("error") != ""
            or not isinstance(tool_version, str)
            or not tool_version
            or not isinstance(results_count, int)
            or results_count < 0
        ):
            raise ReleaseContractError(
                f"{required.tool_name} code-scanning analysis is errored or lacks tool version"
            )
        evidence.append(
            {
                "analysis_id": analysis_id,
                "tool_name": required.tool_name,
                "tool_version": tool_version,
                "created_at": analysis["created_at"],
                "commit_sha": target_sha,
                "ref": "refs/heads/main",
                "analysis_key": required.analysis_key,
                "category": required.category,
                "environment": required.environment,
                "error": "",
                "results_count": results_count,
            }
        )
    return evidence


def verify_main_ci(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    *,
    allow_exact_published: bool = False,
) -> dict[str, Any]:
    _validate_repository(repository)
    _validate_sha(target_sha)
    if release_tag != FIRST_RELEASE_TAG:
        raise ReleaseContractError(f"release tag must be {FIRST_RELEASE_TAG}")

    release_state, release_id = _verify_release_state(
        client,
        repository,
        target_sha,
        release_tag,
        allow_exact_published=allow_exact_published,
    )
    if release_state == RELEASE_STATE_PUBLISHED:
        return {
            "schema_version": 1,
            "release_state": RELEASE_STATE_PUBLISHED,
            "release_id": release_id,
            "release_tag": release_tag,
            "target_sha": target_sha,
        }

    main_ref = client.get(f"/repos/{repository}/git/ref/heads/main")
    main_sha = main_ref.get("object", {}).get("sha")
    if main_sha != target_sha:
        raise ReleaseContractError(
            f"target SHA {target_sha} is not the current origin/main SHA {main_sha}"
        )
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
    code_scanning_evidence = _verify_required_code_scanning_analyses(
        client, repository, target_sha
    )
    return {
        "schema_version": 3,
        "target_sha": target_sha,
        "all_triggered_push_workflows": len(exact_runs),
        "required_checks": evidence,
        "required_code_scanning_analyses": code_scanning_evidence,
        "workbench_run_id": workbench_run_id,
    }


def _artifact_name(prefix: str, release_tag: str, target_sha: str) -> str:
    return f"{prefix}-{release_tag}-{target_sha}"


def find_verification_run(
    client: GitHubAPI, repository: str, target_sha: str, release_tag: str
) -> tuple[int, int]:
    _validate_repository(repository)
    _validate_sha(target_sha)
    proof_name = _artifact_name("release-verification", release_tag, target_sha)
    assets_name = _artifact_name("release-assets", release_tag, target_sha)
    artifacts = client.paginate(
        f"/repos/{repository}/actions/artifacts", "artifacts", {"name": proof_name}
    )
    candidates: list[tuple[int, int, int]] = []
    for artifact in artifacts:
        if artifact.get("name") != proof_name or artifact.get("expired") is not False:
            continue
        workflow_run = artifact.get("workflow_run") or {}
        run_id = workflow_run.get("id")
        if (
            workflow_run.get("head_sha") != target_sha
            or type(run_id) is not int
            or run_id <= 0
        ):
            continue
        run = client.get(f"/repos/{repository}/actions/runs/{run_id}")
        run_attempt = run.get("run_attempt")
        if (
            run.get("name") != "Release Artifacts"
            or run.get("path") != RELEASE_WORKFLOW_PATH
            or run.get("event") != "workflow_dispatch"
            or run.get("head_branch") != "main"
            or run.get("head_sha") != target_sha
            or run.get("status") != "completed"
            or run.get("conclusion") != "success"
            or type(run_attempt) is not int
            or run_attempt <= 0
            or run.get("html_url")
            != f"https://github.com/{repository}/actions/runs/{run_id}"
        ):
            continue
        artifact_id = artifact.get("id")
        if type(artifact_id) is int and artifact_id > 0:
            candidates.append((artifact_id, run_id, run_attempt))
    if not candidates:
        raise ReleaseContractError(
            "publish mode requires a completed successful verify-only artifact for the exact SHA"
        )
    selected_artifact_id, selected_run_id, selected_run_attempt = max(candidates)
    run_artifacts = client.paginate(
        f"/repos/{repository}/actions/runs/{selected_run_id}/artifacts", "artifacts"
    )
    matching_proofs = [
        artifact
        for artifact in run_artifacts
        if artifact.get("name") == proof_name and artifact.get("expired") is False
    ]
    matching_assets = [
        artifact
        for artifact in run_artifacts
        if artifact.get("name") == assets_name and artifact.get("expired") is False
    ]
    if (
        len(matching_proofs) != 1
        or matching_proofs[0].get("id") != selected_artifact_id
        or len(matching_assets) != 1
    ):
        raise ReleaseContractError(
            f"verification run {selected_run_id} must contain one exact proof artifact "
            f"and one nonexpired {assets_name} artifact"
        )
    return selected_run_id, selected_run_attempt


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


def expected_dependency_source_names() -> tuple[str, ...]:
    urls, digests, roots = _dependency_source_contracts()
    names = tuple(urls)
    if set(names) != set(digests) or set(names) != set(roots):
        raise ReleaseContractError(
            "dependency source URL, digest, and archive-root contracts diverged"
        )
    for name in names:
        endpoints = urls[name]
        if (
            not re.fullmatch(r"[A-Za-z0-9._-]+", name)
            or not isinstance(endpoints, tuple)
            or not endpoints
            or len(endpoints) != len(set(endpoints))
            or any(not endpoint.startswith("https://") for endpoint in endpoints)
            or not SHA256_PATTERN.fullmatch(digests[name])
        ):
            raise ReleaseContractError(f"invalid dependency source contract for {name}")
    return names


def expected_gnfs_source_name(release_tag: str) -> str:
    if release_tag != FIRST_RELEASE_TAG:
        raise ReleaseContractError(f"release tag must be {FIRST_RELEASE_TAG}")
    return f"gnfs-{release_tag}-source.tar.gz"


def expected_corresponding_source_names(release_tag: str) -> tuple[str, ...]:
    return (expected_gnfs_source_name(release_tag), *expected_dependency_source_names())


def expected_release_asset_names(release_tag: str) -> tuple[str, ...]:
    return (*expected_package_names(release_tag), *expected_corresponding_source_names(release_tag))


def _release_asset_identity_contract(release_tag: str) -> dict[str, tuple[str, str]]:
    package_names = expected_package_names(release_tag)
    contract = {
        package_names[0]: ("macos-application", "macos-arm64"),
        package_names[1]: ("cli-sdk", "linux-x86_64"),
        package_names[2]: ("cli-sdk", "macos-arm64"),
        package_names[3]: ("cli-sdk", "windows-x86_64"),
    }
    contract.update(
        {
            name: ("corresponding-source", "source")
            for name in expected_corresponding_source_names(release_tag)
        }
    )
    return contract


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
        "contract_file",
        "contract_sha256",
        "dependencies",
        "runtime",
        "schema_version",
    }:
        raise ReleaseContractError("Windows runtime manifest has missing or unknown fields")
    contract = _windows_runtime_contract()
    contract_name = WINDOWS_RUNTIME_CONTRACT_PATH.name
    contract_path = f"{root}/{contract_name}"
    try:
        expected_contract_bytes = WINDOWS_RUNTIME_CONTRACT_PATH.read_bytes()
    except OSError as error:
        raise ReleaseContractError(f"unable to read Windows runtime contract: {error}") from error
    if (
        manifest.get("schema_version") != 2
        or manifest.get("runtime") != contract.runtime
        or manifest.get("contract_file") != contract_name
        or contract_path not in files
        or read_file(contract_path) != expected_contract_bytes
        or manifest.get("contract_sha256")
        != hashlib.sha256(expected_contract_bytes).hexdigest()
    ):
        raise ReleaseContractError("Windows runtime manifest identity is invalid")
    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        raise ReleaseContractError("Windows runtime manifest has no bundled dependencies")
    runtime_by_name = {record.name: record for record in contract.runtime_packages}
    install_by_name = {record.name: record for record in contract.install_packages}
    source_by_name = {record.name: record for record in contract.source_archives}
    observed_dlls: list[str] = []
    declared_licenses: set[str] = set()
    for dependency in dependencies:
        if not isinstance(dependency, dict) or set(dependency) != {
            "dll",
            "license_files",
            "package",
            "package_archive",
            "package_archive_sha256",
            "package_version",
            "sha256",
            "source_archive",
            "source_archive_sha256",
        }:
            raise ReleaseContractError("Windows runtime dependency has missing or unknown fields")
        dll = dependency.get("dll")
        package = dependency.get("package")
        package_version = dependency.get("package_version")
        digest = dependency.get("sha256")
        licenses = dependency.get("license_files")
        package_contract = runtime_by_name.get(package) if isinstance(package, str) else None
        install_contract = install_by_name.get(package) if isinstance(package, str) else None
        source_contract = (
            source_by_name.get(package_contract.source_archive)
            if package_contract is not None
            else None
        )
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
            or package_contract is None
            or install_contract is None
            or source_contract is None
            or package_version != package_contract.version
            or dependency.get("package_archive") != install_contract.archive
            or dependency.get("package_archive_sha256") != install_contract.sha256
            or dependency.get("source_archive") != source_contract.name
            or dependency.get("source_archive_sha256") != source_contract.sha256
            or package_contract.dll_sha256.get(dll) != digest
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
        expected_fallbacks = {
            f"licenses/{package}/{fallback.archive_name}": fallback.sha256
            for fallback in package_contract.fallback_licenses
        }
        for fallback_path, fallback_digest in expected_fallbacks.items():
            if fallback_path not in licenses or hashlib.sha256(
                read_file(f"{root}/{fallback_path}")
            ).hexdigest() != fallback_digest:
                raise ReleaseContractError(
                    f"Windows runtime fallback license is missing or changed: {fallback_path}"
                )
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
    observed_packages = {dependency["package"] for dependency in dependencies}
    if observed_packages != {record.name for record in contract.runtime_packages}:
        raise ReleaseContractError("Windows runtime package set is not the exact source closure")
    expected_dlls = {
        dll for package in contract.runtime_packages for dll in package.dll_sha256
    }
    if set(observed_dlls) != expected_dlls:
        raise ReleaseContractError("Windows runtime DLL set is not the exact release closure")
    notice = _decode_archive_text(read_file, f"{root}/THIRD_PARTY_NOTICES.txt")
    if "runtime-dependencies.json" not in notice:
        raise ReleaseContractError("Windows third-party notice does not identify its manifest")
    for dependency in dependencies:
        identity = f"{dependency['package']} {dependency['package_version']}"
        if identity not in notice:
            raise ReleaseContractError(
                f"Windows third-party notice omits package identity {identity}"
            )
        if dependency["source_archive"] not in notice:
            raise ReleaseContractError(
                f"Windows third-party notice omits source archive {dependency['source_archive']}"
            )
        if (
            dependency["package"] == "mingw-w64-ucrt-x86_64-gmp"
            and "license selection: GNU GPL version 2" not in notice
        ):
            raise ReleaseContractError(
                "Windows third-party notice omits the GMP GNU GPL version 2 selection"
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
        if (
            "UCRT64" not in readme
            or "runtime-dependencies.json" not in readme
            or WINDOWS_RUNTIME_CONTRACT_PATH.name not in readme
        ):
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


def _validate_source_tar_structure(
    archive_path: Path,
    expected_root: str,
    expected_commit: str | None = None,
) -> dict[str, tuple[Any, ...]]:
    try:
        with tarfile.open(archive_path, mode="r:*") as archive:
            if expected_commit is not None and archive.pax_headers.get("comment") != expected_commit:
                raise ReleaseContractError(
                    f"source archive is not bound to exact commit {expected_commit}"
                )
            members = archive.getmembers()
            if not members or len(members) > 100_000:
                raise ReleaseContractError(
                    f"source archive has an invalid entry count: {archive_path.name}"
                )
            names: set[str] = set()
            manifest: dict[str, tuple[Any, ...]] = {}
            has_root_directory = False
            for member in members:
                normalized = member.name.rstrip("/")
                path = PurePosixPath(normalized)
                if (
                    not normalized
                    or normalized in names
                    or path.is_absolute()
                    or ".." in path.parts
                    or "\\" in member.name
                    or "\0" in member.name
                    or not path.parts
                    or path.parts[0] != expected_root
                ):
                    raise ReleaseContractError(
                        f"source archive contains an unsafe or unexpected path: {member.name}"
                    )
                names.add(normalized)
                if normalized == expected_root and member.isdir():
                    has_root_directory = True
                if member.isdir():
                    manifest[normalized] = ("directory", member.mode & 0o777)
                    continue
                if not member.isfile():
                    raise ReleaseContractError(
                        f"source archive contains a link or special entry: {member.name}"
                    )
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise ReleaseContractError(
                        f"source archive file cannot be read: {member.name}"
                    )
                digest = hashlib.sha256()
                with extracted:
                    for chunk in iter(lambda: extracted.read(1024 * 1024), b""):
                        digest.update(chunk)
                manifest[normalized] = (
                    "file",
                    member.mode & 0o777,
                    member.size,
                    digest.hexdigest(),
                )
            if not has_root_directory:
                raise ReleaseContractError(
                    f"source archive lacks exact top-level directory {expected_root}"
                )
            return manifest
    except tarfile.TarError as error:
        raise ReleaseContractError(
            f"invalid source archive {archive_path.name}: {error}"
        ) from error


def _repository_head(repository_root: Path) -> tuple[Path, str]:
    root = repository_root.resolve()
    if not root.is_dir() or repository_root.is_symlink():
        raise ReleaseContractError(f"repository root must be a real directory: {root}")
    try:
        top_level = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except subprocess.CalledProcessError as error:
        raise ReleaseContractError(f"unable to resolve source repository: {error}") from error
    if Path(top_level).resolve() != root or not FULL_SHA_PATTERN.fullmatch(head):
        raise ReleaseContractError("source repository identity is invalid")
    return root, head


def _write_git_source_archive(
    repository_root: Path,
    target_sha: str,
    root_name: str,
    archive_format: str,
    output: Path,
) -> None:
    command = [
        "git",
        "archive",
        f"--format={archive_format}",
        f"--prefix={root_name}/",
        target_sha,
    ]
    with output.open("xb") as handle:
        result = subprocess.run(
            command,
            cwd=repository_root,
            check=False,
            stdout=handle,
            stderr=subprocess.PIPE,
            text=True,
        )
    if result.returncode != 0:
        raise ReleaseContractError(
            "unable to create exact-SHA GNFS source archive: "
            f"{result.stderr.strip()[:1000]}"
        )


def validate_gnfs_source_archive(
    archive_path: Path,
    repository_root: Path,
    target_sha: str,
    release_tag: str,
) -> None:
    _validate_sha(target_sha)
    expected_name = expected_gnfs_source_name(release_tag)
    if archive_path.name != expected_name or archive_path.is_symlink():
        raise ReleaseContractError(
            f"GNFS source archive name does not match the release contract: {archive_path.name}"
        )
    archive_path = archive_path.resolve()
    if not archive_path.is_file():
        raise ReleaseContractError(f"GNFS source archive is missing: {archive_path}")
    root, head = _repository_head(repository_root)
    if head != target_sha:
        raise ReleaseContractError(
            f"source repository HEAD is {head}, expected exact release SHA {target_sha}"
        )
    source_root = expected_name[: -len(".tar.gz")]
    observed_manifest = _validate_source_tar_structure(
        archive_path, source_root, expected_commit=target_sha
    )
    with tempfile.TemporaryDirectory(prefix="gnfs-source-contract-") as temp_dir:
        expected_archive = Path(temp_dir) / "expected.tar"
        _write_git_source_archive(root, target_sha, source_root, "tar", expected_archive)
        expected_manifest = _validate_source_tar_structure(
            expected_archive, source_root, expected_commit=target_sha
        )
    if observed_manifest != expected_manifest:
        raise ReleaseContractError(
            "GNFS source archive contents do not match the exact target commit"
        )


def create_gnfs_source_archive(
    output_directory: Path,
    repository_root: Path,
    target_sha: str,
    release_tag: str,
) -> Path:
    _validate_sha(target_sha)
    root, head = _repository_head(repository_root)
    if head != target_sha:
        raise ReleaseContractError(
            f"source repository HEAD is {head}, expected exact release SHA {target_sha}"
        )
    output_directory = output_directory.resolve()
    if output_directory.exists() or output_directory.is_symlink():
        raise ReleaseContractError(
            f"refusing to reuse GNFS source directory: {output_directory}"
        )
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    output_directory.mkdir()
    archive_name = expected_gnfs_source_name(release_tag)
    archive_path = output_directory / archive_name
    partial_path = output_directory / f".{archive_name}.partial"
    root_name = archive_name[: -len(".tar.gz")]
    _write_git_source_archive(root, target_sha, root_name, "tar.gz", partial_path)
    os.replace(partial_path, archive_path)
    validate_gnfs_source_archive(archive_path, root, target_sha, release_tag)
    return archive_path


def _validate_msys2_source_archive(path: Path, expected_root: str) -> None:
    try:
        completed = subprocess.run(
            ["tar", "--zstd", "-tf", str(path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ReleaseContractError(
            f"unable to inspect MSYS2 source archive {path.name}: {error}"
        ) from error
    if completed.returncode != 0:
        raise ReleaseContractError(
            f"MSYS2 source archive cannot be listed: {path.name}: {completed.stderr[:500]}"
        )
    names = completed.stdout.splitlines()
    if not names or len(names) > 200_000 or names != list(dict.fromkeys(names)):
        raise ReleaseContractError(
            f"MSYS2 source archive has an invalid entry set: {path.name}"
        )
    normalized_names: set[str] = set()
    for name in names:
        archive_path = PurePosixPath(name)
        if (
            not archive_path.parts
            or archive_path.is_absolute()
            or ".." in archive_path.parts
            or "\\" in name
            or "\0" in name
            or archive_path.parts[0] != expected_root
        ):
            raise ReleaseContractError(
                f"MSYS2 source archive contains an unsafe path: {path.name}: {name}"
            )
        normalized_names.add(archive_path.as_posix())
    source = next(
        (
            record
            for record in _windows_runtime_contract().source_archives
            if record.name == path.name
        ),
        None,
    )
    if source is None or source.root != expected_root:
        raise ReleaseContractError(f"MSYS2 source archive is not in the runtime contract: {path.name}")
    required = {f"{expected_root}/{relative}" for relative in source.required_paths}
    if not required.issubset(normalized_names):
        raise ReleaseContractError(
            f"MSYS2 source archive lacks required recipe/source paths: {path.name}: "
            f"{sorted(required - normalized_names)}"
        )


def _validate_dependency_source_files(source_directory: Path) -> None:
    _, digests, roots = _dependency_source_contracts()
    for name in expected_dependency_source_names():
        path = source_directory / name
        if not path.is_file() or path.is_symlink():
            raise ReleaseContractError(f"dependency source archive is not a real file: {name}")
        observed_digest = _sha256(path)
        expected_digest = digests[name]
        if observed_digest != expected_digest:
            raise ReleaseContractError(
                f"dependency source archive digest mismatch for {name}: {observed_digest}"
            )
        if name.endswith(".src.tar.zst"):
            _validate_msys2_source_archive(path, roots[name])
        else:
            _validate_source_tar_structure(path, roots[name])


def _validate_corresponding_source_files(
    source_directory: Path, target_sha: str, release_tag: str
) -> None:
    repository_root = Path(__file__).resolve().parents[1]
    validate_gnfs_source_archive(
        source_directory / expected_gnfs_source_name(release_tag),
        repository_root,
        target_sha,
        release_tag,
    )
    _validate_dependency_source_files(source_directory)


def validate_dependency_source_archives(source_directory: Path) -> None:
    source_directory = source_directory.resolve()
    expected_names = expected_dependency_source_names()
    observed_names = _flat_file_names(
        source_directory, "dependency source archive directory"
    )
    if observed_names != sorted(expected_names):
        raise ReleaseContractError(
            "dependency source archive directory is missing, extra, or renamed: "
            f"{observed_names}"
        )
    _validate_dependency_source_files(source_directory)


def fetch_dependency_source_archives(output_directory: Path) -> None:
    output_directory = output_directory.resolve()
    if output_directory.exists() or output_directory.is_symlink():
        raise ReleaseContractError(
            f"refusing to reuse dependency source directory: {output_directory}"
        )
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    output_directory.mkdir()
    ssl_context = _verified_ssl_context()
    urls, digests, _ = _dependency_source_contracts()
    for name in expected_dependency_source_names():
        endpoints = urls[name]
        partial_path = output_directory / f".{name}.partial"
        expected_digest = digests[name]
        last_error: Exception | None = None
        for attempt in range(1, 4):
            endpoint = endpoints[(attempt - 1) % len(endpoints)]
            request = Request(
                endpoint,
                headers={
                    "Accept": "application/octet-stream",
                    "User-Agent": "gnfs-release-source-fetch/1",
                },
            )
            digest = hashlib.sha256()
            size = 0
            try:
                with urlopen(request, timeout=120, context=ssl_context) as response:
                    final_url = response.geturl()
                    if not isinstance(final_url, str) or not final_url.startswith("https://"):
                        raise ReleaseContractError(
                            f"dependency source download left HTTPS for {name}: {final_url}"
                        )
                    content_length_header = response.headers.get("Content-Length")
                    content_length = (
                        int(content_length_header)
                        if content_length_header is not None
                        else None
                    )
                    if content_length is not None and (
                        content_length <= 0 or content_length > MAX_DEPENDENCY_SOURCE_BYTES
                    ):
                        raise ReleaseContractError(
                            f"dependency source archive has invalid Content-Length: {name}"
                        )
                    with partial_path.open("wb") as handle:
                        while True:
                            chunk = response.read(1024 * 1024)
                            if not chunk:
                                break
                            size += len(chunk)
                            if size > MAX_DEPENDENCY_SOURCE_BYTES:
                                raise ReleaseContractError(
                                    f"dependency source archive exceeds size cap: {name}"
                                )
                            digest.update(chunk)
                            handle.write(chunk)
                    if content_length is not None and size != content_length:
                        raise ReleaseContractError(
                            f"dependency source download was truncated for {name}: "
                            f"{size} of {content_length} bytes"
                        )
                    observed_digest = digest.hexdigest()
                    if observed_digest != expected_digest:
                        raise ReleaseContractError(
                            "dependency source download digest mismatch for "
                            f"{name}: {observed_digest}"
                        )
            except (
                IncompleteRead,
                OSError,
                ReleaseContractError,
                ValueError,
            ) as error:
                last_error = error
                partial_path.unlink(missing_ok=True)
                if attempt < 3:
                    continue
                raise ReleaseContractError(
                    "dependency source download failed after three attempts across "
                    f"{len(endpoints)} pinned HTTPS endpoint(s) for {name}: {error}"
                ) from error
            os.replace(partial_path, output_directory / name)
            break
        else:
            raise ReleaseContractError(
                f"dependency source download failed for {name}: {last_error}"
            )
    validate_dependency_source_archives(output_directory)


def assemble_release_bundle(
    asset_directory: Path, target_sha: str, release_tag: str, source_date_epoch: int
) -> None:
    _validate_sha(target_sha)
    expected_source_date_epoch = _target_commit_epoch(target_sha)
    if source_date_epoch != expected_source_date_epoch:
        raise ReleaseContractError(
            "source date epoch must equal the exact target commit timestamp "
            f"{expected_source_date_epoch}"
        )
    asset_directory = asset_directory.resolve()
    package_names = expected_package_names(release_tag)
    release_asset_names = expected_release_asset_names(release_tag)
    existing = _flat_file_names(asset_directory, "release assembly directory")
    if existing != sorted(release_asset_names):
        raise ReleaseContractError(
            "release assembly requires the exact binary and corresponding-source assets; "
            f"found {existing}"
        )
    _validate_workbench_zip(asset_directory / package_names[0], target_sha, release_tag)
    for platform, name in zip(
        ("linux-x86_64", "macos-arm64", "windows-x86_64"), package_names[1:]
    ):
        validate_cli_archive(asset_directory / name, platform, release_tag)
    _validate_corresponding_source_files(asset_directory, target_sha, release_tag)

    records = [
        _asset_record(asset_directory / name, kind, platform)
        for name, (kind, platform) in _release_asset_identity_contract(release_tag).items()
    ]
    records.sort(key=lambda record: record["name"])
    metadata = {
        "schema_version": 2,
        "release_tag": release_tag,
        "target_sha": target_sha,
        "source_date_epoch": source_date_epoch,
        "assets": records,
        "publication_evidence": {
            "name": RELEASE_PROOF_NAME,
            "kind": "evidence",
            "digest_binding": "publication-contract",
        },
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

    checksum_paths = [asset_directory / name for name in release_asset_names]
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
    release_asset_names = expected_release_asset_names(release_tag)
    expected_files = sorted((*release_asset_names, "release-metadata.json", "SHA256SUMS"))
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
    _validate_corresponding_source_files(asset_directory, target_sha, release_tag)

    metadata = _read_json_object(asset_directory / "release-metadata.json")
    expected_metadata_keys = {
        "schema_version",
        "release_tag",
        "target_sha",
        "source_date_epoch",
        "assets",
        "publication_evidence",
        "workbench_security",
    }
    if set(metadata) != expected_metadata_keys:
        raise ReleaseContractError("release metadata contains missing or unknown fields")
    if (
        metadata.get("schema_version") != 2
        or metadata.get("release_tag") != release_tag
        or metadata.get("target_sha") != target_sha
        or not isinstance(metadata.get("source_date_epoch"), int)
        or metadata["source_date_epoch"] != _target_commit_epoch(target_sha)
    ):
        raise ReleaseContractError("release metadata identity is invalid")
    if metadata.get("publication_evidence") != {
        "name": RELEASE_PROOF_NAME,
        "kind": "evidence",
        "digest_binding": "publication-contract",
    }:
        raise ReleaseContractError("release metadata lost its publication evidence identity")
    if metadata.get("workbench_security") != {
        "signing": "ad-hoc",
        "notarized": False,
        "source_revision_key": WORKBENCH_INFO_KEY,
    }:
        raise ReleaseContractError("release metadata must disclose ad-hoc, unnotarized Workbench")

    assets = metadata.get("assets")
    if not isinstance(assets, list) or len(assets) != len(release_asset_names):
        raise ReleaseContractError(
            "release metadata must describe every binary and corresponding-source asset"
        )
    asset_names = [asset.get("name") for asset in assets if isinstance(asset, dict)]
    if asset_names != sorted(release_asset_names):
        raise ReleaseContractError("release metadata assets are not canonical and sorted")
    identity_contract = _release_asset_identity_contract(release_tag)
    for asset in assets:
        if not isinstance(asset, dict) or set(asset) != {
            "name",
            "kind",
            "platform",
            "sha256",
            "size",
        }:
            raise ReleaseContractError("release metadata asset contains unknown fields")
        name = asset["name"]
        if not isinstance(name, str) or (asset["kind"], asset["platform"]) != identity_contract.get(
            name
        ):
            raise ReleaseContractError("release metadata asset identity changed")
        path = asset_directory / name
        if (
            not SHA256_PATTERN.fullmatch(str(asset["sha256"]))
            or asset["sha256"] != _sha256(path)
            or asset["size"] != path.stat().st_size
        ):
            raise ReleaseContractError(f"release metadata digest mismatch for {path.name}")

    checksum_path = asset_directory / "SHA256SUMS"
    expected_checksum_names = sorted((*release_asset_names, "release-metadata.json"))
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


def _required_code_scanning_contract() -> list[dict[str, Any]]:
    return [asdict(required) for required in REQUIRED_CODE_SCANNING_ANALYSES]


def _validate_main_ci_evidence(evidence: dict[str, Any], target_sha: str) -> None:
    expected_keys = {
        "schema_version",
        "target_sha",
        "all_triggered_push_workflows",
        "required_checks",
        "required_code_scanning_analyses",
        "workbench_run_id",
    }
    if set(evidence) != expected_keys:
        raise ReleaseContractError("main CI evidence contains missing or unknown fields")
    if (
        evidence.get("schema_version") != 3
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

    code_scanning = evidence.get("required_code_scanning_analyses")
    if not isinstance(code_scanning, list) or len(code_scanning) != len(
        REQUIRED_CODE_SCANNING_ANALYSES
    ):
        raise ReleaseContractError("main CI evidence has the wrong code-scanning count")
    for required, record in zip(REQUIRED_CODE_SCANNING_ANALYSES, code_scanning):
        if not isinstance(record, dict) or set(record) != {
            "analysis_id",
            "tool_name",
            "tool_version",
            "created_at",
            "commit_sha",
            "ref",
            "analysis_key",
            "category",
            "environment",
            "error",
            "results_count",
        }:
            raise ReleaseContractError("main CI code-scanning evidence record is malformed")
        if (
            record.get("tool_name") != required.tool_name
            or record.get("commit_sha") != target_sha
            or record.get("ref") != "refs/heads/main"
            or record.get("analysis_key") != required.analysis_key
            or record.get("category") != required.category
            or record.get("environment") != required.environment
            or record.get("error") != ""
            or not isinstance(record.get("analysis_id"), int)
            or record["analysis_id"] <= 0
            or not isinstance(record.get("tool_version"), str)
            or not record["tool_version"]
            or not isinstance(record.get("created_at"), str)
            or not isinstance(record.get("results_count"), int)
            or record["results_count"] < 0
        ):
            raise ReleaseContractError("main CI code-scanning evidence changed identity or result")
        try:
            created = datetime.fromisoformat(record["created_at"].replace("Z", "+00:00"))
        except ValueError as error:
            raise ReleaseContractError(
                "main CI CodeQL evidence has invalid created_at"
            ) from error
        if created.tzinfo is None:
            raise ReleaseContractError("main CI CodeQL evidence timestamp lacks timezone")


def write_verification_proof(
    asset_directory: Path,
    output: Path,
    ci_evidence_path: Path,
    target_sha: str,
    release_tag: str,
    repository: str,
    workflow_run_id: int,
    workflow_run_attempt: int,
    server_url: str,
) -> None:
    verify_release_bundle(asset_directory, target_sha, release_tag)
    ci_evidence = _read_json_object(ci_evidence_path)
    _validate_main_ci_evidence(ci_evidence, target_sha)
    _validate_repository(repository)
    if (
        type(workflow_run_id) is not int
        or workflow_run_id <= 0
        or type(workflow_run_attempt) is not int
        or workflow_run_attempt <= 0
    ):
        raise ReleaseContractError("verification workflow run identity must be positive")
    if server_url != "https://github.com":
        raise ReleaseContractError("verification workflow server must be https://github.com")
    workflow_run_url = f"{server_url}/{repository}/actions/runs/{workflow_run_id}"
    output = output.resolve()
    if output.exists() or output.is_symlink():
        raise ReleaseContractError(f"refusing to overwrite verification proof: {output}")
    bundle_names = sorted(
        (*expected_release_asset_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    proof = {
        "schema_version": 3,
        "mode": "verify-only",
        "release_tag": release_tag,
        "target_sha": target_sha,
        "required_main_checks": _required_check_contract(),
        "required_code_scanning_analyses": _required_code_scanning_contract(),
        "verification_workflow": {
            "run_id": workflow_run_id,
            "run_attempt": workflow_run_attempt,
            "url": workflow_run_url,
        },
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
    proof_path: Path,
    asset_directory: Path,
    target_sha: str,
    release_tag: str,
    repository: str,
    expected_workflow_run_id: int,
    expected_workflow_run_attempt: int,
) -> None:
    verify_release_bundle(asset_directory, target_sha, release_tag)
    proof = _read_json_object(proof_path)
    expected_keys = {
        "schema_version",
        "mode",
        "release_tag",
        "target_sha",
        "required_main_checks",
        "required_code_scanning_analyses",
        "verification_workflow",
        "main_ci_evidence",
        "bundle",
    }
    if set(proof) != expected_keys:
        raise ReleaseContractError("verification proof contains missing or unknown fields")
    if (
        proof.get("schema_version") != 3
        or proof.get("mode") != "verify-only"
        or proof.get("release_tag") != release_tag
        or proof.get("target_sha") != target_sha
        or proof.get("required_main_checks") != _required_check_contract()
        or proof.get("required_code_scanning_analyses")
        != _required_code_scanning_contract()
    ):
        raise ReleaseContractError("verification proof identity or required checks changed")
    _validate_repository(repository)
    if (
        type(expected_workflow_run_id) is not int
        or expected_workflow_run_id <= 0
        or type(expected_workflow_run_attempt) is not int
        or expected_workflow_run_attempt <= 0
    ):
        raise ReleaseContractError("selected verification workflow identity must be positive")
    workflow = proof.get("verification_workflow")
    if not isinstance(workflow, dict) or set(workflow) != {"run_id", "run_attempt", "url"}:
        raise ReleaseContractError("verification proof workflow identity is malformed")
    if (
        type(workflow.get("run_id")) is not int
        or workflow.get("run_id") != expected_workflow_run_id
        or type(workflow.get("run_attempt")) is not int
        or workflow.get("run_attempt") != expected_workflow_run_attempt
        or workflow["run_attempt"] <= 0
        or workflow.get("url")
        != f"https://github.com/{repository}/actions/runs/{expected_workflow_run_id}"
    ):
        raise ReleaseContractError("verification proof does not match the selected workflow run")
    main_ci_evidence = proof.get("main_ci_evidence")
    if not isinstance(main_ci_evidence, dict):
        raise ReleaseContractError("verification proof lacks main CI evidence")
    _validate_main_ci_evidence(main_ci_evidence, target_sha)
    expected_names = sorted(
        (*expected_release_asset_names(release_tag), "release-metadata.json", "SHA256SUMS")
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


def _public_release_files(
    asset_directory: Path,
    proof_path: Path,
    target_sha: str,
    release_tag: str,
    repository: str,
    verification_workflow_run_id: int,
    verification_workflow_run_attempt: int,
) -> dict[str, Path]:
    if proof_path.is_symlink():
        raise ReleaseContractError("release verification proof must not be a symlink")
    verify_verification_proof(
        proof_path,
        asset_directory,
        target_sha,
        release_tag,
        repository,
        verification_workflow_run_id,
        verification_workflow_run_attempt,
    )
    proof_path = proof_path.resolve()
    if proof_path.name != RELEASE_PROOF_NAME or not proof_path.is_file():
        raise ReleaseContractError("release verification proof must be a real canonical file")
    names = sorted(
        (*expected_release_asset_names(release_tag), "release-metadata.json", "SHA256SUMS")
    )
    files = {name: asset_directory.resolve() / name for name in names}
    files[RELEASE_PROOF_NAME] = proof_path
    return files


def _release_notes(target_sha: str, release_tag: str) -> str:
    return "\n".join(
        (
            f"# GNFS {release_tag}",
            "",
            (
                "The first public GNFS release delivers the C++20 factoring library and "
                "CLI/SDK for Linux, macOS, and Windows together with the native macOS Workbench."
            ),
            "",
            f"Source revision: `{target_sha}`",
            "",
            "## Highlights",
            "",
            "- Complete factoring through trial division, Pollard rho, SIQS, and GNFS.",
            (
                "- Recursive complete factorization and a versioned JSON Lines event stream "
                "for GUIs and automation."
            ),
            (
                "- Native SwiftUI Workbench with progress, relation metrics, structured logs, "
                "complete prime factors, and local history."
            ),
            (
                "- Exact-SHA, two-phase qualification with cross-platform CI, CodeQL, a "
                "120-bit structured-filter lane, and a bounded 50-digit route comparison."
            ),
            (
                "- Reproducible archive metadata, corresponding sources, SHA-256 manifests, "
                "and immutable publication checks."
            ),
            "",
            "## Choose an asset",
            "",
            "| Use case | Asset | Runtime boundary |",
            "|---|---|---|",
            (
                "| macOS application | `GNFSWorkbench-0.1.0-macOS-arm64.zip` | "
                "Apple silicon; macOS 26+ |"
            ),
            (
                "| Linux CLI/SDK | `gnfs-v0.1.0-linux-x86_64.tar.gz` | "
                "x86_64; glibc 2.31+; host GMP/NTL |"
            ),
            (
                "| macOS CLI/SDK | `gnfs-v0.1.0-macos-arm64.tar.gz` | "
                "Apple silicon; macOS 13+; host GMP/NTL |"
            ),
            (
                "| Windows CLI/SDK | `gnfs-v0.1.0-windows-x86_64.zip` | "
                "x86_64; MSYS2 UCRT64; four pinned DLLs included |"
            ),
            (
                "| Exact project source | `gnfs-v0.1.0-source.tar.gz` | Generated from and "
                "validated against the source revision above |"
            ),
            "",
            (
                "GMP 6.3.0, NTL 11.6.0, and the exact MSYS2 GCC, GMP, and winpthreads "
                "corresponding-source archives are attached for the binary dependency closure."
            ),
            "",
            "Corresponding-source assets:",
            "",
            "- `gmp-6.3.0.tar.xz`",
            "- `ntl-11.6.0.tar.gz`",
            "- `mingw-w64-gcc-16.1.0-6.src.tar.zst`",
            "- `mingw-w64-gmp-6.3.0-2.src.tar.zst`",
            "- `mingw-w64-winpthreads-14.0.0.r220.gd999af622-1.src.tar.zst`",
            "",
            "## Verify the download",
            "",
            (
                "`SHA256SUMS` contains the SHA-256 for all application, CLI/SDK, and source "
                "archives plus `release-metadata.json`. For one downloaded asset:"
            ),
            "",
            "```bash",
            "asset=GNFSWorkbench-0.1.0-macOS-arm64.zip",
            "line=\"$(awk -v name=\"$asset\" '$2 == name { print }' SHA256SUMS)\"",
            "test -n \"$line\"",
            "if command -v sha256sum >/dev/null 2>&1; then",
            "  printf '%s\\n' \"$line\" | sha256sum -c -",
            "else",
            "  printf '%s\\n' \"$line\" | shasum -a 256 -c -",
            "fi",
            "```",
            "",
            (
                "`release-metadata.json` binds every package/source asset to this source "
                "revision. `release-verification.json` binds the exact main CI and verify-only "
                "evidence plus every other release asset; its own local and server SHA-256 are "
                "checked separately by the publication contract."
            ),
            "",
            "## Security and compatibility notes",
            "",
            (
                "- The Workbench is ad-hoc signed and is not Apple notarized. macOS may require "
                "explicit approval before first launch."
            ),
            (
                "- Linux and macOS do not bundle the dynamically linked GMP/NTL libraries. "
                "Check the archive README for the observed ABI contract."
            ),
            (
                "- Windows bundles only the four contracted UCRT64 runtime DLLs and includes "
                "their licenses and corresponding sources."
            ),
            (
                "- SHA-256 proves byte equality with the unsigned manifest; this release does "
                "not claim a detached signature, signed SLSA provenance, or a separate "
                "SPDX/CycloneDX SBOM."
            ),
            "",
            (
                "See README.md or README-EN.md for installation and PowerShell verification "
                "instructions, and docs/releasing.md for the complete 13-asset publication "
                "contract."
            ),
            "",
        )
    )


def _verify_prepared_draft(
    release: dict[str, Any],
    expected_release_id: int,
    target_sha: str,
    release_tag: str,
) -> None:
    if _verify_resumable_draft_identity(release, target_sha, release_tag) != expected_release_id:
        raise ReleaseContractError("draft release id changed during preparation")
    if release.get("name") != release_tag or release.get("body") != _release_notes(
        target_sha, release_tag
    ):
        raise ReleaseContractError("draft release title or notes changed")


def _assert_tag_absent(client: GitHubAPI, repository: str, release_tag: str) -> None:
    encoded_tag = quote(release_tag, safe="")
    if client.get_optional(f"/repos/{repository}/git/ref/tags/{encoded_tag}") is not None:
        raise ReleaseContractError("release tag appeared before draft publication")


def _index_release_asset_records(
    assets: Any,
    local_files: dict[str, Path],
    *,
    require_complete: bool,
) -> dict[str, dict[str, Any]]:
    if not isinstance(assets, list):
        raise ReleaseContractError("release server asset response is not a list")
    assets_by_name: dict[str, dict[str, Any]] = {}
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise ReleaseContractError("release contains an invalid server asset record")
        name = asset["name"]
        if name in assets_by_name:
            raise ReleaseContractError(f"release contains duplicate server asset {name}")
        if name not in local_files:
            raise ReleaseContractError(f"release contains unexpected server asset {name}")
        path = local_files[name]
        if (
            asset.get("state") != "uploaded"
            or asset.get("size") != path.stat().st_size
            or asset.get("digest") != f"sha256:{_sha256(path)}"
        ):
            raise ReleaseContractError(
                f"release server asset bytes do not match verified local bytes: {name}"
            )
        assets_by_name[name] = asset
    if require_complete and sorted(assets_by_name) != sorted(local_files):
        raise ReleaseContractError("release server asset set is incomplete")
    return assets_by_name


def prepare_draft_release(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    asset_directory: Path,
    proof_path: Path,
    verification_workflow_run_id: int,
    verification_workflow_run_attempt: int,
) -> dict[str, Any]:
    """Create or resume one exact draft, or verify one frozen public release."""

    _validate_repository(repository)
    _validate_sha(target_sha)
    local_files = _public_release_files(
        asset_directory,
        proof_path,
        target_sha,
        release_tag,
        repository,
        verification_workflow_run_id,
        verification_workflow_run_attempt,
    )

    def recovered_public_result(release_id: int) -> dict[str, Any]:
        frozen = _verify_frozen_public_release(
            client,
            repository,
            release_id,
            target_sha,
            release_tag,
            local_files,
        )
        return {
            **frozen,
            "created": False,
            "uploaded": [],
            "recovered": True,
        }

    release_state, observed_release_id = _verify_release_state(
        client,
        repository,
        target_sha,
        release_tag,
        allow_exact_published=True,
    )
    if release_state == RELEASE_STATE_PUBLISHED:
        if type(observed_release_id) is not int:
            raise ReleaseContractError("published release recovery lost its numeric id")
        return recovered_public_result(observed_release_id)

    created = False
    if release_state == RELEASE_STATE_DRAFT:
        if type(observed_release_id) is not int:
            raise ReleaseContractError("resumable draft lost its numeric id")
        release_id = observed_release_id
    else:
        payload = {
            "tag_name": release_tag,
            "target_commitish": target_sha,
            "name": release_tag,
            "body": _release_notes(target_sha, release_tag),
            "draft": True,
            "prerelease": False,
            "generate_release_notes": False,
        }
        try:
            created_release = client.post(f"/repos/{repository}/releases", payload)
            release_id = _verify_resumable_draft_identity(
                created_release, target_sha, release_tag
            )
            created = True
        except GitHubAPIRequestError as error:
            if error.status != 422:
                raise
            raced_state, raced_release_id = _verify_release_state(
                client,
                repository,
                target_sha,
                release_tag,
                allow_exact_published=True,
            )
            if raced_state == RELEASE_STATE_PUBLISHED:
                if type(raced_release_id) is not int:
                    raise ReleaseContractError(
                        "concurrent publication lost its numeric release id"
                    ) from error
                return recovered_public_result(raced_release_id)
            if raced_state != RELEASE_STATE_DRAFT or type(raced_release_id) is not int:
                raise ReleaseContractError(
                    "draft creation raced without one exact resumable draft"
                ) from error
            release_id = raced_release_id

    draft = client.get(f"/repos/{repository}/releases/{release_id}")
    _verify_prepared_draft(draft, release_id, target_sha, release_tag)
    uploaded_names: list[str] = []
    for name, path in sorted(local_files.items()):
        _assert_tag_absent(client, repository, release_tag)
        draft = client.get(f"/repos/{repository}/releases/{release_id}")
        _verify_prepared_draft(draft, release_id, target_sha, release_tag)
        current_assets = client.get(
            f"/repos/{repository}/releases/{release_id}/assets",
            {"per_page": "100"},
        )
        indexed = _index_release_asset_records(
            current_assets, local_files, require_complete=False
        )
        if name in indexed:
            continue
        try:
            uploaded = client.upload_release_asset(repository, release_id, name, path)
            _index_release_asset_records([uploaded], {name: path}, require_complete=True)
            uploaded_names.append(name)
        except GitHubAPIRequestError as error:
            if error.status != 422:
                raise
            raced_assets = client.get(
                f"/repos/{repository}/releases/{release_id}/assets",
                {"per_page": "100"},
            )
            raced_index = _index_release_asset_records(
                raced_assets, local_files, require_complete=False
            )
            if name not in raced_index:
                raise ReleaseContractError(
                    f"asset upload raced without exact server bytes: {name}"
                ) from error

    _assert_tag_absent(client, repository, release_tag)
    final_draft = client.get(f"/repos/{repository}/releases/{release_id}")
    _verify_prepared_draft(final_draft, release_id, target_sha, release_tag)
    final_assets = client.get(
        f"/repos/{repository}/releases/{release_id}/assets",
        {"per_page": "100"},
    )
    _index_release_asset_records(final_assets, local_files, require_complete=True)
    return {
        "schema_version": 1,
        "release_id": release_id,
        "created": created,
        "uploaded": uploaded_names,
        "asset_count": len(local_files),
        "immutable": False,
        "recovered": False,
    }


def _verify_release_tag_record(
    tag_ref: Any, release_tag: str, target_sha: str
) -> None:
    if not isinstance(tag_ref, dict):
        raise ReleaseContractError("release tag response is not an object")
    if (
        tag_ref.get("ref") != f"refs/tags/{release_tag}"
        or tag_ref.get("object", {}).get("type") != "commit"
        or tag_ref.get("object", {}).get("sha") != target_sha
    ):
        raise ReleaseContractError("release tag is not an exact lightweight ref to target SHA")


def _verify_release_tag(
    client: GitHubAPI, repository: str, release_tag: str, target_sha: str
) -> None:
    encoded_tag = quote(release_tag, safe="")
    _verify_release_tag_record(
        client.get(f"/repos/{repository}/git/ref/tags/{encoded_tag}"),
        release_tag,
        target_sha,
    )


def _verify_release_identity(
    release: dict[str, Any],
    *,
    expected_release_id: int,
    release_tag: str,
    target_sha: str,
    draft: bool,
    immutable: bool,
) -> None:
    if (
        not isinstance(release, dict)
        or type(release.get("id")) is not int
        or release.get("id") != expected_release_id
        or release.get("draft") is not draft
        or release.get("prerelease") is not False
        or release.get("immutable") is not immutable
        or release.get("tag_name") != release_tag
        or release.get("target_commitish") != target_sha
        or release.get("name") != release_tag
        or release.get("body") != _release_notes(target_sha, release_tag)
    ):
        state = "draft" if draft else "public immutable"
        raise ReleaseContractError(f"release is not the exact target-SHA {state} release")


def _verify_release_asset_records(assets: Any, local_files: dict[str, Path]) -> None:
    _index_release_asset_records(assets, local_files, require_complete=True)


def _verify_server_release_assets(
    client: GitHubAPI,
    repository: str,
    expected_release_id: int,
    local_files: dict[str, Path],
) -> None:
    assets = client.get(
        f"/repos/{repository}/releases/{expected_release_id}/assets",
        {"per_page": "100"},
    )
    _verify_release_asset_records(assets, local_files)


def _verify_frozen_public_release(
    client: GitHubAPI,
    repository: str,
    expected_release_id: int,
    target_sha: str,
    release_tag: str,
    local_files: dict[str, Path],
) -> dict[str, Any]:
    """Re-read and bind every immutable public-release representation."""

    encoded_tag = quote(release_tag, safe="")
    public_by_id = client.get(f"/repos/{repository}/releases/{expected_release_id}")
    public_by_tag = client.get(f"/repos/{repository}/releases/tags/{encoded_tag}")
    for public_release in (public_by_id, public_by_tag):
        _verify_release_identity(
            public_release,
            expected_release_id=expected_release_id,
            release_tag=release_tag,
            target_sha=target_sha,
            draft=False,
            immutable=True,
        )
        _verify_release_asset_records(public_release.get("assets"), local_files)
    _verify_release_tag(client, repository, release_tag, target_sha)
    _verify_server_release_assets(client, repository, expected_release_id, local_files)
    return {
        "schema_version": 1,
        "release_id": expected_release_id,
        "release_tag": release_tag,
        "target_sha": target_sha,
        "immutable": True,
        "asset_count": len(local_files),
    }


def publish_verified_release(
    client: GitHubAPI,
    repository: str,
    target_sha: str,
    release_tag: str,
    expected_release_id: int,
    asset_directory: Path,
    proof_path: Path,
    verification_workflow_run_id: int,
    verification_workflow_run_attempt: int,
) -> dict[str, Any]:
    """Publish one exact draft, or recover one exact frozen publication."""

    if type(expected_release_id) is not int or expected_release_id <= 0:
        raise ReleaseContractError("expected draft release id must be positive")
    local_files = _public_release_files(
        asset_directory,
        proof_path,
        target_sha,
        release_tag,
        repository,
        verification_workflow_run_id,
        verification_workflow_run_attempt,
    )
    release_state, observed_release_id = _verify_release_state(
        client,
        repository,
        target_sha,
        release_tag,
        allow_exact_published=True,
    )
    if release_state == RELEASE_STATE_PUBLISHED:
        if observed_release_id != expected_release_id:
            raise ReleaseContractError(
                "published release id does not match the prepared numeric release id"
            )
        frozen = _verify_frozen_public_release(
            client,
            repository,
            expected_release_id,
            target_sha,
            release_tag,
            local_files,
        )
        return {**frozen, "recovered": True}
    if (
        release_state != RELEASE_STATE_DRAFT
        or observed_release_id != expected_release_id
    ):
        raise ReleaseContractError("publication requires the exact prepared draft release id")

    protection_before = verify_repository_protection(
        client, repository, allow_unreadable_immutable_setting=True
    )
    ci_before = verify_main_ci(
        client,
        repository,
        target_sha,
        release_tag,
    )
    proof = _read_json_object(proof_path)
    if proof.get("main_ci_evidence") != ci_before:
        raise ReleaseContractError("live main CI evidence changed from the verification proof")
    draft_release = client.get(
        f"/repos/{repository}/releases/{expected_release_id}"
    )
    _verify_prepared_draft(
        draft_release, expected_release_id, target_sha, release_tag
    )
    _assert_tag_absent(client, repository, release_tag)
    _verify_server_release_assets(client, repository, expected_release_id, local_files)
    draft_release = client.get(
        f"/repos/{repository}/releases/{expected_release_id}"
    )
    _verify_prepared_draft(
        draft_release, expected_release_id, target_sha, release_tag
    )
    _assert_tag_absent(client, repository, release_tag)

    published_response = client.patch(
        f"/repos/{repository}/releases/{expected_release_id}",
        {"draft": False, "prerelease": False},
    )
    _verify_release_identity(
        published_response,
        expected_release_id=expected_release_id,
        release_tag=release_tag,
        target_sha=target_sha,
        draft=False,
        immutable=True,
    )
    _verify_release_asset_records(published_response.get("assets"), local_files)

    # Everything below is fetched again after the mutating call. A successful
    # PATCH response alone is not publication evidence.
    frozen = _verify_frozen_public_release(
        client,
        repository,
        expected_release_id,
        target_sha,
        release_tag,
        local_files,
    )
    return {
        **frozen,
        "recovered": False,
        "protection_before": protection_before,
        "ci_before": ci_before,
    }


def _canonical_workflow_on_block(workflow_text: str) -> str:
    lines = workflow_text.splitlines(keepends=True)
    top_level_key = re.compile(
        r'^(?:"(?P<double>[A-Za-z0-9_-]+)"|'
        r"'(?P<single>[A-Za-z0-9_-]+)'|(?P<plain>[A-Za-z0-9_-]+))\s*:"
    )
    on_indices: list[int] = []
    for index, line in enumerate(lines):
        if re.match(r"^<<\s*:", line):
            raise ReleaseContractError("workflow root mappings must not use YAML merges")
        match = top_level_key.match(line)
        if match and next(value for value in match.groups() if value is not None) == "on":
            on_indices.append(index)
    if len(on_indices) != 1:
        raise ReleaseContractError(
            "workflow must contain exactly one canonical top-level on mapping; "
            f"found {len(on_indices)}"
        )
    on_index = on_indices[0]
    if lines[on_index] != "on:\n":
        raise ReleaseContractError("workflow top-level on mapping spelling is not canonical")
    body: list[str] = []
    for line in lines[on_index + 1 :]:
        content = line.lstrip(" ")
        indentation = len(line) - len(content)
        if content.strip() and not content.startswith("#") and indentation == 0:
            break
        body.append(line)
    return "".join(body)


def _workflow_trigger_block(workflow_text: str, trigger: str) -> str:
    lines = _canonical_workflow_on_block(workflow_text).splitlines(keepends=True)
    trigger_key = re.compile(
        r"^ {2}(?:\"(?P<double>[A-Za-z0-9_-]+)\"|"
        r"'(?P<single>[A-Za-z0-9_-]+)'|(?P<plain>[A-Za-z0-9_-]+))\s*:"
    )
    trigger_indices: list[int] = []
    for index, line in enumerate(lines):
        if re.match(r"^ {2}<<\s*:", line):
            raise ReleaseContractError("workflow trigger mappings must not use YAML merges")
        match = trigger_key.match(line)
        if match and next(value for value in match.groups() if value is not None) == trigger:
            trigger_indices.append(index)
    if len(trigger_indices) != 1:
        raise ReleaseContractError(
            f"workflow must contain exactly one canonical {trigger} trigger; "
            f"found {len(trigger_indices)}"
        )
    trigger_index = trigger_indices[0]
    if lines[trigger_index] != f"  {trigger}:\n":
        raise ReleaseContractError(f"workflow {trigger} trigger spelling is not canonical")
    body: list[str] = []
    for line in lines[trigger_index + 1 :]:
        content = line.lstrip(" ")
        indentation = len(line) - len(content)
        if content.strip() and not content.startswith("#") and indentation < 4:
            break
        body.append(line)
    return "".join(body)


def _validate_required_main_push_trigger(workflow_path: Path, workflow_text: str) -> None:
    push_block = _workflow_trigger_block(workflow_text, "push")
    canonical_blocks = {"    branches: [ main ]\n", "    branches: [ main, dev ]\n"}
    if push_block not in canonical_blocks:
        raise ReleaseContractError(
            f"required release workflow {workflow_path} must use one canonical, "
            "unfiltered main push block"
        )


def validate_workflow_sources(release_workflow: Path, qualification_workflow: Path) -> None:
    release_text = release_workflow.read_text(encoding="utf-8")
    qualification_text = qualification_workflow.read_text(encoding="utf-8")
    readiness_workflow = release_workflow.with_name("release-readiness.yml")
    readiness_text = readiness_workflow.read_text(encoding="utf-8")
    required_workflow_paths = sorted(
        {Path(required.workflow_path) for required in REQUIRED_MAIN_CHECKS}
    )
    for workflow_path in required_workflow_paths:
        workflow_file = release_workflow.parent / workflow_path.name
        workflow_text = workflow_file.read_text(encoding="utf-8")
        _validate_required_main_push_trigger(workflow_path, workflow_text)
    forbidden_release_fragments = (
        "if: always()",
        "--clobber",
        "continue-on-error:",
        "github.ref }}",
        "github.ref_name",
        "inputs.tag",
        "mapfile -t runtime_dlls",
        "--method PATCH",
        "gh release create",
        "add-apt-repository",
        "software-properties-common",
        "group: release-${{ inputs.release_tag }}-${{ inputs.target_sha }}",
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
        "group: release-${{ inputs.release_tag }}",
        "scripts/release_contract.py verify-main",
        "scripts/release_contract.py find-verification",
        "scripts/release_contract.py create-gnfs-source",
        "scripts/release_contract.py validate-gnfs-source",
        "scripts/release_contract.py fetch-sources",
        "scripts/release_contract.py validate-sources",
        "scripts/release_contract.py verify-proof",
        "scripts/release_contract.py verify-protection",
        "scripts/release_contract.py prepare-draft",
        "scripts/release_contract.py publish-release",
        "security-events: read",
        "release-main-ci-evidence-${{ github.run_id }}",
        "--ci-evidence main-ci-evidence/main-ci-evidence.json",
        "--workflow-run-id \"${WORKFLOW_RUN_ID}\"",
        "--workflow-run-attempt \"${WORKFLOW_RUN_ATTEMPT}\"",
        "--server-url \"${SERVER_URL}\"",
        "--expected-workflow-run-attempt \"${VERIFICATION_WORKFLOW_RUN_ATTEMPT}\"",
        "--verification-workflow-run-attempt \"${VERIFICATION_WORKFLOW_RUN_ATTEMPT}\"",
        "verification_run_attempt: ${{ steps.prior.outputs.verification_run_attempt }}",
        "source_date_epoch=$(git show -s --format=%ct HEAD)",
        "scripts/release_binary_contract.py linux",
        "scripts/release_binary_contract.py macos",
        "scripts/windows_release_runtime.py bundle",
        "scripts/windows_release_runtime.py install-pinned",
        "--pinned-package-evidence pinned-windows-packages.json",
        "-DGNFS_ENABLE_NTL=OFF",
        "container: ubuntu:20.04@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214",
        "Acquire::Retries=4",
        "https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/",
        "C8EC952E2A0E1FBDC5090F6A2C277A0A352154E5",
        "Suites: focal",
        "Architectures: amd64",
        "Signed-By: /etc/apt/keyrings/ubuntu-toolchain-r-test.gpg",
        "getconf GNU_LIBC_VERSION",
        "gcc-12",
        "g++-12",
        "-DCMAKE_CXX_COMPILER=g++-12",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0",
        "release-verification-${{ inputs.release_tag }}-${{ inputs.target_sha }}",
        "gnfs-project-source",
        "gnfs-dependency-sources",
        "verification-proof/release-verification.json",
        "--proof verification-proof/release-verification.json",
        "--github-output \"${GITHUB_OUTPUT}\"",
        "steps.main-ci.outputs.release_state != 'published-immutable'",
    )
    for fragment in required_release_fragments:
        if fragment not in release_text:
            raise ReleaseContractError(f"release workflow lost required boundary: {fragment}")
    if release_text.count("--allow-exact-published") != 2:
        raise ReleaseContractError(
            "release workflow must allow exact published recovery only in both publish preflights"
        )
    if "release_state_args+=(--allow-exact-published)" not in release_text:
        raise ReleaseContractError(
            "release workflow lost its publish-only exact-public preflight boundary"
        )
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
    if "if: always()" in readiness_text or "continue-on-error:" in readiness_text:
        raise ReleaseContractError("release readiness contains a fail-open job boundary")
    if "mingw-w64-ucrt-x86_64-ntl" in release_text + readiness_text:
        raise ReleaseContractError("Windows release workflows must not install MSYS2 NTL")
    if re.search(r"(?m)^\s+paths(?:-ignore)?:\s*$", readiness_text):
        raise ReleaseContractError(
            "required release readiness must run for every main and pull-request SHA"
        )
    required_readiness_fragments = (
        "name: Release Readiness",
        "name: Windows pinned runtime and source closure",
        "runs-on: windows-2022",
        "scripts/windows_release_runtime.py install-pinned",
        "scripts/windows_release_runtime.py bundle",
        "--pinned-package-evidence pinned-windows-packages.json",
        "-DGNFS_ENABLE_NTL=OFF",
        "scripts/release_contract.py verify-checkout",
        "steps.source.outputs.epoch",
        "scripts/release_contract.py validate-cli-archive",
        "scripts/reproducible_archive.py create",
    )
    for fragment in required_readiness_fragments:
        if fragment not in readiness_text:
            raise ReleaseContractError(
                f"release readiness workflow lost required boundary: {fragment}"
            )


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
        self.code_scanning_analyses: list[dict[str, Any]] = [
            {
                "id": 9000,
                "created_at": "2026-08-03T22:56:50Z",
                "ref": "refs/heads/main",
                "commit_sha": target_sha,
                "analysis_key": ".github/workflows/codeql.yml:analyze",
                "category": ".github/workflows/codeql.yml:analyze",
                "environment": "{}",
                "error": "",
                "results_count": 0,
                "tool": {"name": "CodeQL", "version": "2.26.2"},
            }
        ]
        self.proof_artifacts: list[dict[str, Any]] = []
        self.run_details: dict[int, dict[str, Any]] = {}
        self.run_artifacts: dict[int, list[dict[str, Any]]] = {}
        self.tag_ref: dict[str, Any] | None = None
        self.release: dict[str, Any] | None = None
        self.extra_releases: list[dict[str, Any]] = []
        self.ruleset: dict[str, Any] = {
            "id": RELEASE_TAG_RULESET_ID,
            "node_id": RELEASE_TAG_RULESET_NODE_ID,
            "name": RELEASE_TAG_RULESET_NAME,
            "target": "tag",
            "source_type": "Repository",
            "source": "example/GNFS",
            "enforcement": "active",
            "bypass_actors": [],
            "current_user_can_bypass": "never",
            "conditions": {
                "ref_name": {"include": ["refs/tags/v*"], "exclude": []},
            },
            "rules": [{"type": "update"}, {"type": "deletion"}],
            "created_at": RELEASE_TAG_RULESET_CREATED_AT,
            "updated_at": RELEASE_TAG_RULESET_UPDATED_AT,
        }
        self.immutable_setting_readable = True
        self.immutable_setting_enabled = True
        self.publish_immutable = True
        self.post_patch_mutator: Callable[[], None] | None = None
        self.before_upload_mutator: Callable[[str, Path], None] | None = None
        self.uploaded_names: list[str] = []
        self.patch_count = 0
        self.create_count = 0
        self.next_release_id = 7000
        self.release_read_id_override: int | None = None
        self.fail_next_release_get_status: int | None = None
        self.post_create_race: Callable[[dict[str, Any]], None] | None = None

    def get(self, path: str, query: dict[str, str] | None = None) -> Any:
        if path.endswith("/git/ref/heads/main"):
            return {"object": {"sha": self.target_sha}}
        if path.endswith(f"/rulesets/{RELEASE_TAG_RULESET_ID}"):
            return self.ruleset
        if path.endswith("/immutable-releases"):
            if not self.immutable_setting_readable:
                raise GitHubAPIRequestError(403, path, "Resource not accessible by integration")
            if not self.immutable_setting_enabled:
                raise GitHubAPIRequestError(404, path, "Not Found")
            return {"enabled": True, "enforced_by_owner": False}
        if "/git/ref/tags/" in path:
            if self.tag_ref is None:
                raise GitHubAPIRequestError(404, path, "Not Found")
            return self.tag_ref
        if "/releases/tags/" in path:
            if self.release is None or self.release.get("draft") is True:
                raise GitHubAPIRequestError(404, path, "Not Found")
            return self.release
        if path.endswith("/assets") and self.release is not None:
            if query != {"per_page": "100"}:
                raise AssertionError("release asset verification must request 100 records")
            return self.release["assets"]
        match = re.search(r"/releases/([1-9][0-9]*)$", path)
        if match and self.release is not None:
            if self.fail_next_release_get_status is not None:
                status = self.fail_next_release_get_status
                self.fail_next_release_get_status = None
                raise GitHubAPIRequestError(status, path, "transient release read failure")
            if int(match.group(1)) != self.release.get("id"):
                raise GitHubAPIRequestError(404, path, "Not Found")
            if self.release_read_id_override is None:
                return self.release
            response = json.loads(json.dumps(self.release))
            response["id"] = self.release_read_id_override
            return response
        match = re.search(r"/actions/runs/(\d+)$", path)
        if match:
            return self.run_details[int(match.group(1))]
        raise AssertionError(f"unexpected fake GET {path}")

    def patch(self, path: str, payload: dict[str, Any]) -> Any:
        if self.release is None or path != f"/repos/example/GNFS/releases/{self.release['id']}":
            raise AssertionError(f"unexpected fake PATCH {path}")
        if payload != {"draft": False, "prerelease": False}:
            raise AssertionError(f"unexpected fake PATCH payload {payload}")
        self.patch_count += 1
        self.release["draft"] = False
        self.release["prerelease"] = False
        self.release["immutable"] = self.publish_immutable
        self.tag_ref = {
            "ref": f"refs/tags/{self.release['tag_name']}",
            "object": {"type": "commit", "sha": self.release["target_commitish"]},
        }
        response = json.loads(json.dumps(self.release))
        if self.post_patch_mutator is not None:
            self.post_patch_mutator()
        return response

    def post(self, path: str, payload: dict[str, Any]) -> Any:
        self.create_count += 1
        if self.post_create_race is not None:
            mutator = self.post_create_race
            self.post_create_race = None
            mutator(payload)
            raise GitHubAPIRequestError(422, path, "Validation Failed")
        if path != "/repos/example/GNFS/releases" or self.release is not None:
            raise GitHubAPIRequestError(422, path, "Validation Failed")
        self.release = {
            "id": self.next_release_id,
            "name": payload.get("name"),
            "body": payload.get("body"),
            "draft": payload.get("draft"),
            "prerelease": payload.get("prerelease"),
            "immutable": False,
            "tag_name": payload.get("tag_name"),
            "target_commitish": payload.get("target_commitish"),
            "assets": [],
        }
        return json.loads(json.dumps(self.release))

    def upload_release_asset(
        self, repository: str, release_id: int, name: str, path: Path
    ) -> Any:
        if repository != "example/GNFS" or self.release is None:
            raise AssertionError("unexpected fake asset upload repository")
        if release_id != self.release.get("id"):
            raise GitHubAPIRequestError(404, "fake-upload", "Not Found")
        if self.before_upload_mutator is not None:
            mutator = self.before_upload_mutator
            self.before_upload_mutator = None
            mutator(name, path)
        if any(asset.get("name") == name for asset in self.release["assets"]):
            raise GitHubAPIRequestError(422, "fake-upload", "already_exists")
        asset = {
            "id": 10_000 + len(self.release["assets"]),
            "name": name,
            "state": "uploaded",
            "size": path.stat().st_size,
            "digest": f"sha256:{_sha256(path)}",
        }
        self.release["assets"].append(asset)
        self.uploaded_names.append(name)
        return json.loads(json.dumps(asset))

    def get_optional(self, path: str) -> Any | None:
        if "/git/ref/tags/" in path:
            return self.tag_ref
        if "/releases/tags/" in path:
            if self.release is not None and self.release.get("draft") is False:
                return self.release
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
                raise AssertionError("check verification must request every check run")
            return self.checks
        if path.endswith("/actions/artifacts") and query:
            return self.proof_artifacts
        match = re.search(r"/actions/runs/(\d+)/artifacts$", path)
        if match:
            return self.run_artifacts[int(match.group(1))]
        raise AssertionError(f"unexpected fake pagination {path}")

    def paginate_array(
        self, path: str, query: dict[str, str] | None = None
    ) -> list[Any]:
        if path.endswith("/releases"):
            return ([self.release] if self.release is not None else []) + self.extra_releases
        if path.endswith("/code-scanning/analyses"):
            if query != {"ref": "refs/heads/main"}:
                raise AssertionError("CodeQL analysis must be filtered to main")
            return self.code_scanning_analyses
        raise AssertionError(f"unexpected fake array pagination {path}")


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
        contract = _windows_runtime_contract()
        installs = {package.name: package for package in contract.install_packages}
        sources = {source.name: source for source in contract.source_archives}
        dependencies: list[dict[str, Any]] = []
        notice_lines = ["runtime-dependencies.json", WINDOWS_RUNTIME_CONTRACT_PATH.name]
        for package in contract.runtime_packages:
            dll_name, expected_digest = next(iter(package.dll_sha256.items()))
            dll = root / "bin" / dll_name
            dll.write_bytes(f"{package.name} runtime fixture".encode())
            if _sha256(dll) != expected_digest:
                raise ReleaseContractError(
                    f"Windows runtime fixture digest diverged for {dll_name}"
                )
            license_relative = f"licenses/{package.name}/COPYING"
            license_path = root / license_relative
            license_path.parent.mkdir(parents=True)
            license_path.write_text("license fixture\n", encoding="utf-8")
            install = installs[package.name]
            source = sources[package.source_archive]
            dependencies.append(
                {
                    "dll": dll_name,
                    "license_files": [license_relative],
                    "package": package.name,
                    "package_archive": install.archive,
                    "package_archive_sha256": install.sha256,
                    "package_version": package.version,
                    "sha256": expected_digest,
                    "source_archive": source.name,
                    "source_archive_sha256": source.sha256,
                }
            )
            notice_lines.extend(
                (f"{package.name} {package.version}", source.name)
            )
            if package.name == "mingw-w64-ucrt-x86_64-gmp":
                notice_lines.append("license selection: GNU GPL version 2")
        dependencies.sort(key=lambda item: item["dll"].lower())
        contract_bytes = WINDOWS_RUNTIME_CONTRACT_PATH.read_bytes()
        (root / WINDOWS_RUNTIME_CONTRACT_PATH.name).write_bytes(contract_bytes)
        manifest = {
            "contract_file": WINDOWS_RUNTIME_CONTRACT_PATH.name,
            "contract_sha256": hashlib.sha256(contract_bytes).hexdigest(),
            "dependencies": dependencies,
            "runtime": contract.runtime,
            "schema_version": 2,
        }
        (root / "runtime-dependencies.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (root / "README-release.txt").write_text(
            "GNFS v0.1.0 Windows UCRT64 runtime-dependencies.json "
            f"{WINDOWS_RUNTIME_CONTRACT_PATH.name}\n",
            encoding="utf-8",
        )
        (root / "THIRD_PARTY_NOTICES.txt").write_text(
            "\n".join(notice_lines) + "\n",
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
    repository_root, target_sha = _repository_head(Path(__file__).resolve().parents[1])
    repository = "example/GNFS"
    workflow_ref = f"{repository}/{RELEASE_WORKFLOW_PATH}@refs/heads/main"
    valid_required_trigger = """name: Required Fixture

on:
  push:
    branches: [ main, dev ]
  pull_request:
    branches: [ main ]
    paths: [ 'src/**' ]
"""
    fixture_path = Path(".github/workflows/required-fixture.yml")
    _validate_required_main_push_trigger(fixture_path, valid_required_trigger)
    invalid_required_triggers = {
        "filtered push": valid_required_trigger.replace(
            "    branches: [ main, dev ]\n",
            "    branches: [ main, dev ]\n    paths: [ 'src/**' ]\n",
            1,
        ),
        "ignored push paths": valid_required_trigger.replace(
            "    branches: [ main, dev ]\n",
            "    branches: [ main, dev ]\n    paths-ignore: [ 'docs/**' ]\n",
            1,
        ),
        "missing main": valid_required_trigger.replace(
            "    branches: [ main, dev ]", "    branches: [ dev ]", 1
        ),
        "lookalike main branch": valid_required_trigger.replace(
            "    branches: [ main, dev ]", "    branches: [ not-main, dev ]", 1
        ),
        "excluded main": valid_required_trigger.replace(
            "    branches: [ main, dev ]", "    branches: [ '**', '!main' ]", 1
        ),
        "main in comment": valid_required_trigger.replace(
            "    branches: [ main, dev ]", "    branches: [ dev ] # main", 1
        ),
        "quoted paths key": valid_required_trigger.replace(
            "    branches: [ main, dev ]\n",
            "    branches: [ main, dev ]\n    \"paths\": [ 'src/**' ]\n",
            1,
        ),
        "spaced paths key": valid_required_trigger.replace(
            "    branches: [ main, dev ]\n",
            "    branches: [ main, dev ]\n    paths : [ 'src/**' ]\n",
            1,
        ),
        "comment-separated paths": valid_required_trigger.replace(
            "    branches: [ main, dev ]\n",
            "    branches: [ main, dev ]\n  # still inside push\n    paths: [ 'src/**' ]\n",
            1,
        ),
        "duplicate push": valid_required_trigger
        + "  push:\n    branches: [ main, dev ]\n",
        "quoted duplicate push": valid_required_trigger
        + "  \"push\":\n    branches: [ main, dev ]\n",
        "merged trigger mapping": valid_required_trigger
        + "  <<: *additional-events\n",
        "flow-style on with block scalar decoy": """name: Required Fixture

run-name: |
  push:
    branches: [ main, dev ]
on: { push: { branches: [ main ], paths: [ 'docs/**' ] } }
""",
        "pull request on with block scalar decoy": """name: Required Fixture

run-name: |
  push:
    branches: [ main, dev ]
on:
  pull_request:
    branches: [ main ]
""",
        "quoted on mapping": valid_required_trigger.replace("on:\n", '"on":\n', 1),
        "spaced on mapping": valid_required_trigger.replace("on:\n", "on :\n", 1),
        "aliased on mapping": valid_required_trigger.replace(
            "on:\n", "on: *workflow-events\n", 1
        ),
        "duplicate on mapping": valid_required_trigger
        + "on:\n  push:\n    branches: [ main, dev ]\n",
        "merged root mapping": valid_required_trigger + "<<: *workflow-root\n",
    }
    for label, invalid_trigger in invalid_required_triggers.items():
        try:
            _validate_required_main_push_trigger(fixture_path, invalid_trigger)
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                f"required workflow trigger self-test accepted {label}"
            )
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
    release_notes = _release_notes(target_sha, FIRST_RELEASE_TAG)
    required_note_fragments = {
        target_sha,
        *expected_release_asset_names(FIRST_RELEASE_TAG),
        "SHA256SUMS",
        "release-metadata.json",
        RELEASE_PROOF_NAME,
        "ad-hoc signed",
        "not Apple notarized",
        "signed SLSA provenance",
        "SPDX/CycloneDX SBOM",
    }
    missing_note_fragments = sorted(
        fragment for fragment in required_note_fragments if fragment not in release_notes
    )
    if missing_note_fragments:
        raise ReleaseContractError(
            f"release notes self-test found missing disclosure: {missing_note_fragments}"
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

    codeql_analysis = client.code_scanning_analyses[0]

    def expect_codeql_rejection(label: str) -> None:
        try:
            verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
        except ReleaseContractError:
            return
        raise ReleaseContractError(f"main CI self-test accepted {label} CodeQL analysis")

    codeql_analysis["error"] = "analysis failed"
    expect_codeql_rejection("errored")
    codeql_analysis["error"] = ""
    codeql_analysis["tool"]["name"] = "Wrong Tool"
    expect_codeql_rejection("wrong-tool")
    codeql_analysis["tool"]["name"] = "CodeQL"
    codeql_analysis["tool"]["version"] = ""
    expect_codeql_rejection("missing-version")
    codeql_analysis["tool"]["version"] = "2.26.2"
    codeql_analysis["results_count"] = -1
    expect_codeql_rejection("negative-results")
    codeql_analysis["results_count"] = 0
    saved_analysis = client.code_scanning_analyses.pop()
    expect_codeql_rejection("missing")
    client.code_scanning_analyses.append(saved_analysis)
    newer_analysis = json.loads(json.dumps(saved_analysis))
    newer_analysis["id"] += 1
    newer_analysis["created_at"] = "2026-08-03T23:00:00Z"
    client.code_scanning_analyses.append(newer_analysis)
    rerun_evidence = verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
    if rerun_evidence["required_code_scanning_analyses"][0]["analysis_id"] != 9001:
        raise ReleaseContractError("main CI evidence did not select the latest CodeQL rerun")
    newer_analysis["error"] = "rerun failed"
    expect_codeql_rejection("latest errored rerun")
    newer_analysis["error"] = ""
    newer_analysis["commit_sha"] = "9" * 40
    expect_codeql_rejection("latest analysis for a different SHA")
    client.code_scanning_analyses.pop()
    ambiguous_analysis = json.loads(json.dumps(saved_analysis))
    client.code_scanning_analyses.append(ambiguous_analysis)
    expect_codeql_rejection("ambiguous duplicate ordering key")
    client.code_scanning_analyses.pop()

    proof_run_id = 3000
    proof_run_attempt = 1
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
        "run_attempt": proof_run_attempt,
        "html_url": f"https://github.com/{repository}/actions/runs/{proof_run_id}",
    }
    client.run_artifacts[proof_run_id] = [
        {"id": 99, "name": proof_name, "expired": False},
        {"id": 100, "name": assets_name, "expired": False}
    ]
    if find_verification_run(
        client, repository, target_sha, FIRST_RELEASE_TAG
    ) != (proof_run_id, proof_run_attempt):
        raise ReleaseContractError("verification-run self-test selected the wrong run")

    production_source_urls = {
        "gmp-6.3.0.tar.xz": (
            "https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz",
            "https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz",
        ),
        "ntl-11.6.0.tar.gz": ("https://libntl.org/ntl-11.6.0.tar.gz",),
    }
    production_source_hashes = {
        "gmp-6.3.0.tar.xz": "a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898",
        "ntl-11.6.0.tar.gz": "bc0ef9aceb075a6a0673ac8d8f47d5f8458c72fe806e4468fbd5d3daff056182",
    }
    production_source_roots = {
        "gmp-6.3.0.tar.xz": "gmp-6.3.0",
        "ntl-11.6.0.tar.gz": "ntl-11.6.0",
    }
    if (
        DEPENDENCY_SOURCE_URLS != production_source_urls
        or DEPENDENCY_SOURCE_SHA256 != production_source_hashes
        or DEPENDENCY_SOURCE_ROOTS != production_source_roots
    ):
        raise ReleaseContractError("pinned dependency source contract changed")
    production_windows_contract = _windows_runtime_contract()

    def source_archive_fixture(root_name: str, mode: str) -> bytes:
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode=mode) as archive:
            root_info = tarfile.TarInfo(root_name)
            root_info.type = tarfile.DIRTYPE
            root_info.mode = 0o755
            archive.addfile(root_info)
            content = f"{root_name} source self-test fixture\n".encode()
            readme = tarfile.TarInfo(f"{root_name}/README")
            readme.mode = 0o644
            readme.size = len(content)
            archive.addfile(readme, io.BytesIO(content))
        return buffer.getvalue()

    source_fixture_payloads = {
        "gmp-6.3.0.tar.xz": source_archive_fixture("gmp-6.3.0", "w:xz"),
        "ntl-11.6.0.tar.gz": source_archive_fixture("ntl-11.6.0", "w:gz"),
    }

    def msys2_source_archive_fixture(source) -> bytes:
        with tempfile.TemporaryDirectory(
            prefix="gnfs-msys2-source-fixture-"
        ) as fixture_directory:
            fixture_root = Path(fixture_directory) / source.root
            fixture_root.mkdir()
            for relative in source.required_paths:
                path = fixture_root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"{relative} source fixture\n", encoding="utf-8")
            archive_path = Path(fixture_directory) / source.name
            completed = subprocess.run(
                [
                    "tar",
                    "--zstd",
                    "-cf",
                    str(archive_path),
                    "-C",
                    fixture_directory,
                    source.root,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=120,
            )
            if completed.returncode != 0:
                raise ReleaseContractError(
                    "unable to create MSYS2 source self-test fixture: "
                    f"{completed.stderr[:500]}"
                )
            return archive_path.read_bytes()

    fixture_source_urls = dict(production_source_urls)
    fixture_source_roots = dict(production_source_roots)
    for source in production_windows_contract.source_archives:
        source_fixture_payloads[source.name] = msys2_source_archive_fixture(source)
        fixture_source_urls[source.name] = (source.url,)
        fixture_source_roots[source.name] = source.root
    fixture_source_contracts = (
        fixture_source_urls,
        {
            name: hashlib.sha256(payload).hexdigest()
            for name, payload in source_fixture_payloads.items()
        },
        fixture_source_roots,
    )
    fixture_runtime_packages = []
    for package in production_windows_contract.runtime_packages:
        dll_name = next(iter(package.dll_sha256))
        fixture_payload = f"{package.name} runtime fixture".encode()
        fixture_runtime_packages.append(
            replace(
                package,
                dll_sha256={dll_name: hashlib.sha256(fixture_payload).hexdigest()},
                fallback_licenses=(),
            )
        )
    fixture_windows_contract = replace(
        production_windows_contract,
        runtime_packages=tuple(fixture_runtime_packages),
    )
    with patch(
        f"{__name__}._dependency_source_contracts",
        return_value=fixture_source_contracts,
    ), patch(
        f"{__name__}._windows_runtime_contract",
        return_value=fixture_windows_contract,
    ), tempfile.TemporaryDirectory(
        prefix="gnfs-release-contract-self-test-"
    ) as temp_dir:
        root = Path(temp_dir)

        class DependencySourceResponse(io.BytesIO):
            def __init__(self, payload: bytes, url: str) -> None:
                super().__init__(payload)
                self.headers = {"Content-Length": str(len(payload))}
                self._url = url

            def geturl(self) -> str:
                return self._url

        endpoint_names = {
            endpoint: name
            for name, endpoints in fixture_source_urls.items()
            for endpoint in endpoints
        }
        gmp_endpoints = fixture_source_urls["gmp-6.3.0.tar.xz"]
        fallback_calls = 0

        def open_with_gmp_fallback(request, **_kwargs):
            nonlocal fallback_calls
            endpoint = request.full_url
            if endpoint == gmp_endpoints[0] and fallback_calls == 0:
                fallback_calls += 1
                raise TimeoutError("primary source timed out")
            name = endpoint_names.get(endpoint)
            if name is None:
                raise AssertionError(f"unexpected dependency source endpoint: {endpoint}")
            fallback_calls += 1
            return DependencySourceResponse(source_fixture_payloads[name], endpoint)

        fallback_directory = root / "fallback-source-download"
        with patch(f"{__name__}.urlopen", side_effect=open_with_gmp_fallback) as opener:
            fetch_dependency_source_archives(fallback_directory)
            if [call.args[0].full_url for call in opener.call_args_list[:2]] != list(
                gmp_endpoints
            ):
                raise ReleaseContractError(
                    "dependency source download did not fail over to the pinned GMP endpoint"
                )
        if (fallback_directory / "gmp-6.3.0.tar.xz").read_bytes() != (
            source_fixture_payloads["gmp-6.3.0.tar.xz"]
        ) or list(fallback_directory.glob(".*.partial")):
            raise ReleaseContractError(
                "dependency source fallback did not preserve exact bytes and cleanup"
            )

        failed_directory = root / "failed-source-download"
        with patch(f"{__name__}.urlopen", side_effect=OSError("connection reset")) as opener:
            try:
                fetch_dependency_source_archives(failed_directory)
            except ReleaseContractError as error:
                if (
                    "failed after three attempts across 2 pinned HTTPS endpoint(s)"
                    not in str(error)
                    or opener.call_count != 3
                ):
                    raise ReleaseContractError(
                        "dependency source download did not exhaust bounded endpoint retries"
                    ) from error
            else:
                raise ReleaseContractError(
                    "dependency source download accepted a persistent connection failure"
                )
        if list(failed_directory.iterdir()):
            raise ReleaseContractError(
                "dependency source download retained partial bytes after retry exhaustion"
            )

        gmp_only_contract = (
            {"gmp-6.3.0.tar.xz": gmp_endpoints},
            {
                "gmp-6.3.0.tar.xz": hashlib.sha256(
                    source_fixture_payloads["gmp-6.3.0.tar.xz"]
                ).hexdigest()
            },
            {"gmp-6.3.0.tar.xz": "gmp-6.3.0"},
        )

        def open_tampered_source(request, **_kwargs):
            return DependencySourceResponse(b"tampered source", request.full_url)

        digest_mismatch_directory = root / "digest-mismatch-source-download"
        with patch(
            f"{__name__}._dependency_source_contracts",
            return_value=gmp_only_contract,
        ), patch(
            f"{__name__}.urlopen", side_effect=open_tampered_source
        ) as opener:
            try:
                fetch_dependency_source_archives(digest_mismatch_directory)
            except ReleaseContractError as error:
                if "digest mismatch" not in str(error) or opener.call_count != 3:
                    raise ReleaseContractError(
                        "dependency source download did not reject untrusted endpoint bytes"
                    ) from error
            else:
                raise ReleaseContractError(
                    "dependency source download accepted an untrusted endpoint digest"
                )
        if list(digest_mismatch_directory.iterdir()):
            raise ReleaseContractError(
                "dependency source download retained digest-mismatched partial bytes"
            )

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

        dependency_sources = root / "dependency-sources"
        dependency_sources.mkdir()
        for name, payload in source_fixture_payloads.items():
            (dependency_sources / name).write_bytes(payload)
        validate_dependency_source_archives(dependency_sources)

        unsafe_source_archive = root / "unsafe-source.tar.gz"
        with tarfile.open(unsafe_source_archive, mode="w:gz") as archive:
            unsafe_entry = tarfile.TarInfo("../escape")
            unsafe_entry.size = 1
            archive.addfile(unsafe_entry, io.BytesIO(b"x"))
        try:
            _validate_source_tar_structure(unsafe_source_archive, "gmp-6.3.0")
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "dependency source structure validation accepted path traversal"
            )

        def expect_dependency_source_rejection(
            label: str, payloads: dict[str, bytes], expected_error: str
        ) -> None:
            invalid_sources = root / f"bad-dependency-sources-{label}"
            invalid_sources.mkdir()
            for name, payload in payloads.items():
                (invalid_sources / name).write_bytes(payload)
            try:
                validate_dependency_source_archives(invalid_sources)
            except ReleaseContractError as error:
                if expected_error not in str(error):
                    raise ReleaseContractError(
                        f"dependency source {label} self-test failed for the wrong reason: {error}"
                    ) from error
            else:
                raise ReleaseContractError(
                    f"dependency source validation accepted {label} archives"
                )

        expect_dependency_source_rejection(
            "missing",
            {"gmp-6.3.0.tar.xz": source_fixture_payloads["gmp-6.3.0.tar.xz"]},
            "missing, extra, or renamed",
        )
        missing_msys2_source = dict(source_fixture_payloads)
        missing_msys2_source.pop(
            "mingw-w64-winpthreads-14.0.0.r220.gd999af622-1.src.tar.zst"
        )
        expect_dependency_source_rejection(
            "missing-msys2-source",
            missing_msys2_source,
            "missing, extra, or renamed",
        )
        wrong_hash_payloads = dict(source_fixture_payloads)
        wrong_hash_payloads["ntl-11.6.0.tar.gz"] += b"tampered"
        expect_dependency_source_rejection(
            "wrong-hash",
            wrong_hash_payloads,
            "digest mismatch for ntl-11.6.0.tar.gz",
        )

        gnfs_source_directory = root / "gnfs-source"
        gnfs_source_archive = create_gnfs_source_archive(
            gnfs_source_directory,
            repository_root,
            target_sha,
            FIRST_RELEASE_TAG,
        )
        validate_gnfs_source_archive(
            gnfs_source_archive,
            repository_root,
            target_sha,
            FIRST_RELEASE_TAG,
        )
        wrong_gnfs_source_directory = root / "wrong-gnfs-source"
        wrong_gnfs_source_directory.mkdir()
        wrong_gnfs_source = wrong_gnfs_source_directory / expected_gnfs_source_name(
            FIRST_RELEASE_TAG
        )
        source_root = wrong_gnfs_source.name[: -len(".tar.gz")]
        with tarfile.open(
            wrong_gnfs_source,
            mode="w:gz",
            pax_headers={"comment": target_sha},
        ) as archive:
            root_info = tarfile.TarInfo(source_root)
            root_info.type = tarfile.DIRTYPE
            root_info.mode = 0o755
            archive.addfile(root_info)
            content = b"not the target tree\n"
            file_info = tarfile.TarInfo(f"{source_root}/README")
            file_info.mode = 0o644
            file_info.size = len(content)
            archive.addfile(file_info, io.BytesIO(content))
        try:
            validate_gnfs_source_archive(
                wrong_gnfs_source,
                repository_root,
                target_sha,
                FIRST_RELEASE_TAG,
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "GNFS source validation accepted content outside the exact target commit"
            )

        assets = root / "assets"
        assets.mkdir()
        for name, payload in source_fixture_payloads.items():
            (assets / name).write_bytes(payload)
        (assets / gnfs_source_archive.name).write_bytes(gnfs_source_archive.read_bytes())
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

        def expect_windows_archive_rejection(
            label: str,
            transform: Callable[[str, bytes], bytes],
            expected_error: str,
            extra_files: dict[str, bytes] | None = None,
        ) -> None:
            invalid_directory = root / f"bad-windows-{label}"
            invalid_directory.mkdir()
            invalid_archive = invalid_directory / windows_name
            with zipfile.ZipFile(assets / windows_name) as source_archive, zipfile.ZipFile(
                invalid_archive, mode="x", compression=zipfile.ZIP_STORED
            ) as destination_archive:
                for entry in source_archive.infolist():
                    destination_archive.writestr(
                        entry,
                        transform(entry.filename, source_archive.read(entry.filename)),
                    )
                for name, payload in sorted((extra_files or {}).items()):
                    destination_archive.writestr(name, payload)
            try:
                validate_cli_archive(
                    invalid_archive, "windows-x86_64", FIRST_RELEASE_TAG
                )
            except ReleaseContractError as error:
                if expected_error not in str(error):
                    raise ReleaseContractError(
                        f"Windows {label} self-test failed for the wrong reason: {error}"
                    ) from error
            else:
                raise ReleaseContractError(
                    f"Windows archive validation accepted {label}"
                )

        windows_root = windows_name[: -len(".zip")]

        def tamper_gmp_dll(name: str, payload: bytes) -> bytes:
            if name == f"{windows_root}/bin/libgmp-10.dll":
                return payload + b"tampered"
            return payload

        expect_windows_archive_rejection(
            "changed-dll",
            tamper_gmp_dll,
            "Windows runtime DLL digest mismatch",
        )

        def tamper_source_digest(name: str, payload: bytes) -> bytes:
            if name != f"{windows_root}/runtime-dependencies.json":
                return payload
            manifest = json.loads(payload)
            manifest["dependencies"][0]["source_archive_sha256"] = "0" * 64
            return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()

        expect_windows_archive_rejection(
            "changed-source-digest",
            tamper_source_digest,
            "Windows runtime dependency record is invalid",
        )

        def remove_gmp_selection(name: str, payload: bytes) -> bytes:
            if name != f"{windows_root}/THIRD_PARTY_NOTICES.txt":
                return payload
            return payload.replace(
                b"license selection: GNU GPL version 2",
                b"license selection removed",
            )

        expect_windows_archive_rejection(
            "missing-gmp-gpl2-selection",
            remove_gmp_selection,
            "omits the GMP GNU GPL version 2 selection",
        )
        expect_windows_archive_rejection(
            "unexpected-gf2x",
            lambda _name, payload: payload,
            "Windows archive DLL set does not match its runtime manifest",
            {f"{windows_root}/bin/libgf2x-3.dll": b"unexpected GF2X runtime"},
        )

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
        source_date_epoch = _target_commit_epoch(target_sha)
        try:
            assemble_release_bundle(
                assets, target_sha, FIRST_RELEASE_TAG, source_date_epoch + 1
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "release assembly accepted a source epoch different from the target commit"
            )
        assemble_release_bundle(
            assets, target_sha, FIRST_RELEASE_TAG, source_date_epoch
        )
        verify_release_bundle(assets, target_sha, FIRST_RELEASE_TAG)
        metadata_path = assets / "release-metadata.json"
        original_metadata = metadata_path.read_bytes()
        bad_metadata = _read_json_object(metadata_path)
        bad_metadata["source_date_epoch"] = source_date_epoch + 1
        metadata_path.write_text(
            json.dumps(bad_metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        try:
            verify_release_bundle(assets, target_sha, FIRST_RELEASE_TAG)
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "release verification accepted metadata with a non-commit epoch"
            )
        finally:
            metadata_path.write_bytes(original_metadata)
        proof = root / "release-verification.json"
        ci_evidence_path = root / "main-ci-evidence.json"
        ci_evidence_path.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        write_verification_proof(
            assets,
            proof,
            ci_evidence_path,
            target_sha,
            FIRST_RELEASE_TAG,
            repository,
            proof_run_id,
            proof_run_attempt,
            "https://github.com",
        )
        verify_verification_proof(
            proof,
            assets,
            target_sha,
            FIRST_RELEASE_TAG,
            repository,
            proof_run_id,
            proof_run_attempt,
        )
        try:
            verify_verification_proof(
                proof,
                assets,
                target_sha,
                FIRST_RELEASE_TAG,
                repository,
                proof_run_id + 1,
                proof_run_attempt,
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "verification proof accepted a different selected workflow run"
            )

        for label, field, value in (
            ("attempt", "run_attempt", 0),
            ("url", "url", "https://github.com/example/GNFS/actions/runs/9999"),
        ):
            bad_workflow_payload = _read_json_object(proof)
            bad_workflow_payload["verification_workflow"][field] = value
            bad_workflow_proof = root / f"bad-workflow-{label}-proof.json"
            bad_workflow_proof.write_text(
                json.dumps(bad_workflow_payload, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            try:
                verify_verification_proof(
                    bad_workflow_proof,
                    assets,
                    target_sha,
                    FIRST_RELEASE_TAG,
                    repository,
                    proof_run_id,
                    proof_run_attempt,
                )
            except ReleaseContractError:
                pass
            else:
                raise ReleaseContractError(
                    f"verification proof accepted an invalid workflow {label}"
                )
        tampered_proof_payload = _read_json_object(proof)
        tampered_proof_payload["main_ci_evidence"][
            "required_code_scanning_analyses"
        ][0]["analysis_key"] = "wrong-analysis-key"
        tampered_proof = root / "tampered-release-verification.json"
        tampered_proof.write_text(
            json.dumps(tampered_proof_payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        try:
            verify_verification_proof(
                tampered_proof,
                assets,
                target_sha,
                FIRST_RELEASE_TAG,
                repository,
                proof_run_id,
                proof_run_attempt,
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("verification proof accepted altered CodeQL analysis evidence")

        release_id = 7000
        release_files = _public_release_files(
            assets,
            proof,
            target_sha,
            FIRST_RELEASE_TAG,
            repository,
            proof_run_id,
            proof_run_attempt,
        )

        def asset_record(name: str, path: Path) -> dict[str, Any]:
            return {
                "name": name,
                "state": "uploaded",
                "size": path.stat().st_size,
                "digest": f"sha256:{_sha256(path)}",
            }

        def reset_draft(asset_names: list[str] | None = None) -> None:
            client.target_sha = target_sha
            client.tag_ref = None
            client.release = {
                "id": release_id,
                "name": FIRST_RELEASE_TAG,
                "body": _release_notes(target_sha, FIRST_RELEASE_TAG),
                "draft": True,
                "prerelease": False,
                "immutable": False,
                "tag_name": FIRST_RELEASE_TAG,
                "target_commitish": target_sha,
                "assets": [
                    asset_record(name, path)
                    for name, path in sorted(release_files.items())
                    if asset_names is None or name in asset_names
                ],
            }
            client.extra_releases = []
            client.publish_immutable = True
            client.post_patch_mutator = None
            client.before_upload_mutator = None
            client.uploaded_names = []
            client.patch_count = 0
            client.create_count = 0
            client.release_read_id_override = None
            client.fail_next_release_get_status = None
            client.post_create_race = None

        def reset_empty() -> None:
            reset_draft([])
            client.release = None

        def expect_prepare_rejection(label: str) -> None:
            try:
                prepare_draft_release(
                    client,
                    repository,
                    target_sha,
                    FIRST_RELEASE_TAG,
                    assets,
                    proof,
                    proof_run_id,
                    proof_run_attempt,
                )
            except ReleaseContractError:
                return
            raise ReleaseContractError(f"draft preparation accepted {label}")

        release_names = sorted(release_files)
        reset_empty()
        prepared = prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if (
            prepared["created"] is not True
            or prepared["release_id"] != release_id
            or sorted(prepared["uploaded"]) != release_names
            or client.tag_ref is not None
        ):
            raise ReleaseContractError("first draft preparation lost no-tag upload semantics")

        client.uploaded_names = []
        resumed = prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if resumed["created"] is not False or resumed["uploaded"] or client.uploaded_names:
            raise ReleaseContractError("complete draft resume uploaded existing assets")

        partial_names = release_names[:3]
        reset_draft(partial_names)
        resumed = prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if sorted(resumed["uploaded"]) != release_names[3:]:
            raise ReleaseContractError("partial draft resume did not upload only missing assets")

        reset_draft()
        client.release["target_commitish"] = "1" * 40
        expect_prepare_rejection("a wrong target")

        reset_draft()
        client.release_read_id_override = release_id + 1
        expect_prepare_rejection("a mismatched release id response")

        reset_draft()
        client.release["assets"].append(
            {
                "name": "unexpected.bin",
                "state": "uploaded",
                "size": 1,
                "digest": f"sha256:{'1' * 64}",
            }
        )
        expect_prepare_rejection("an extra asset")

        reset_draft()
        client.release["assets"][0]["digest"] = f"sha256:{'2' * 64}"
        expect_prepare_rejection("wrong existing asset bytes")

        raced_name = release_names[0]
        reset_draft(release_names[1:])

        def upload_exact_race(name: str, path: Path) -> None:
            assert client.release is not None and name == raced_name
            client.release["assets"].append(asset_record(name, path))

        client.before_upload_mutator = upload_exact_race
        prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )

        reset_draft(release_names[1:])

        def upload_wrong_race(name: str, path: Path) -> None:
            record = asset_record(name, path)
            record["digest"] = f"sha256:{'3' * 64}"
            assert client.release is not None
            client.release["assets"].append(record)

        client.before_upload_mutator = upload_wrong_race
        expect_prepare_rejection("a wrong-byte concurrent asset")

        reset_empty()

        def create_exact_race(payload: dict[str, Any]) -> None:
            client.release = {
                "id": release_id,
                "name": payload["name"],
                "body": payload["body"],
                "draft": True,
                "prerelease": False,
                "immutable": False,
                "tag_name": payload["tag_name"],
                "target_commitish": payload["target_commitish"],
                "assets": [],
            }

        client.post_create_race = create_exact_race
        raced = prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if raced["created"] is not False or client.tag_ref is not None:
            raise ReleaseContractError("concurrent exact draft creation did not converge")

        reset_draft()
        client.extra_releases = [json.loads(json.dumps(client.release))]
        client.extra_releases[0]["id"] = release_id + 1
        expect_prepare_rejection("multiple same-tag drafts")

        def reset_release() -> None:
            reset_draft()

        def reset_public_release() -> None:
            reset_draft()
            assert client.release is not None
            client.release["draft"] = False
            client.release["prerelease"] = False
            client.release["immutable"] = True
            client.tag_ref = {
                "ref": f"refs/tags/{FIRST_RELEASE_TAG}",
                "object": {"type": "commit", "sha": target_sha},
            }

        def expect_publish_rejection(label: str) -> None:
            try:
                publish_verified_release(
                    client,
                    repository,
                    target_sha,
                    FIRST_RELEASE_TAG,
                    release_id,
                    assets,
                    proof,
                    proof_run_id,
                    proof_run_attempt,
                )
            except ReleaseContractError:
                return
            raise ReleaseContractError(f"publication accepted {label}")

        reset_release()
        client.checks[0]["conclusion"] = "failure"
        expect_publish_rejection("failed exact-SHA CI")
        client.checks[0]["conclusion"] = "success"

        reset_release()
        client.target_sha = "2" * 40
        expect_publish_rejection("a moved main branch")

        reset_release()
        client.tag_ref = {
            "ref": f"refs/tags/{FIRST_RELEASE_TAG}",
            "object": {"type": "commit", "sha": "3" * 40},
        }
        expect_publish_rejection("a tag created before draft publication")

        reset_release()
        client.release["id"] = release_id + 1
        expect_publish_rejection("a different draft release id")

        reset_release()
        client.release["assets"][0]["digest"] = f"sha256:{'4' * 64}"
        expect_publish_rejection("a changed draft asset digest")

        reset_release()
        proof_asset = next(
            asset
            for asset in client.release["assets"]
            if asset["name"] == RELEASE_PROOF_NAME
        )
        proof_asset["digest"] = f"sha256:{'7' * 64}"
        expect_publish_rejection("a changed verification proof digest")

        reset_release()
        client.ruleset["current_user_can_bypass"] = "always"
        expect_publish_rejection("a tag ruleset bypass")
        client.ruleset["current_user_can_bypass"] = "never"

        hidden_bypass = client.ruleset.pop("bypass_actors")
        verify_repository_protection(
            client, repository, allow_unreadable_immutable_setting=False
        )
        client.ruleset["bypass_actors"] = hidden_bypass
        client.ruleset["updated_at"] = "2026-08-03T22:46:29.413Z"
        expect_publish_rejection("a changed tag ruleset version")
        client.ruleset["updated_at"] = RELEASE_TAG_RULESET_UPDATED_AT

        reset_release()
        client.publish_immutable = False
        expect_publish_rejection("immutable false after PATCH")

        reset_release()

        def move_tag_after_patch() -> None:
            assert client.tag_ref is not None
            client.tag_ref["object"]["sha"] = "5" * 40

        client.post_patch_mutator = move_tag_after_patch
        expect_publish_rejection("a post-PATCH tag race")

        reset_release()

        def change_asset_after_patch() -> None:
            assert client.release is not None
            client.release["assets"][0]["digest"] = f"sha256:{'6' * 64}"

        client.post_patch_mutator = change_asset_after_patch
        expect_publish_rejection("a post-PATCH asset race")

        reset_release()

        def change_title_after_patch() -> None:
            assert client.release is not None
            client.release["name"] = "wrong-title"

        client.post_patch_mutator = change_title_after_patch
        expect_publish_rejection("a post-PATCH title race")

        reset_release()

        def change_notes_after_patch() -> None:
            assert client.release is not None
            client.release["body"] = "wrong notes"

        client.post_patch_mutator = change_notes_after_patch
        expect_publish_rejection("a post-PATCH notes race")

        reset_release()

        def move_main_after_patch() -> None:
            client.target_sha = "8" * 40
            client.ruleset["updated_at"] = "2026-08-03T22:46:29.413Z"

        client.post_patch_mutator = move_main_after_patch
        frozen_result = publish_verified_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            release_id,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if frozen_result["immutable"] is not True:
            raise ReleaseContractError("immutable publication lost frozen postconditions")
        client.ruleset["updated_at"] = RELEASE_TAG_RULESET_UPDATED_AT

        reset_release()

        def fail_first_frozen_read_after_patch() -> None:
            client.target_sha = "8" * 40
            client.ruleset["updated_at"] = "2026-08-03T22:46:29.413Z"
            client.fail_next_release_get_status = 503

        client.post_patch_mutator = fail_first_frozen_read_after_patch
        try:
            publish_verified_release(
                client,
                repository,
                target_sha,
                FIRST_RELEASE_TAG,
                release_id,
                assets,
                proof,
                proof_run_id,
                proof_run_attempt,
            )
        except GitHubAPIRequestError as error:
            if error.status != 503:
                raise
        else:
            raise ReleaseContractError(
                "publication self-test did not expose the transient first frozen read"
            )
        if (
            client.patch_count != 1
            or client.release is None
            or client.release.get("draft") is not False
            or client.release.get("immutable") is not True
        ):
            raise ReleaseContractError(
                "transient postcheck failure did not retain one immutable publication"
            )
        try:
            verify_main_ci(client, repository, target_sha, FIRST_RELEASE_TAG)
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError(
                "verify-only main preflight accepted an already published release"
            )
        recovery_preflight = verify_main_ci(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            allow_exact_published=True,
        )
        if (
            recovery_preflight.get("release_state") != RELEASE_STATE_PUBLISHED
            or recovery_preflight.get("release_id") != release_id
        ):
            raise ReleaseContractError(
                "publish preflight did not recognize exact immutable recovery state"
            )
        recovered_prepare = prepare_draft_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        recovered_publish = publish_verified_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            release_id,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if (
            recovered_prepare.get("recovered") is not True
            or recovered_prepare.get("uploaded") != []
            or recovered_publish.get("recovered") is not True
            or client.patch_count != 1
            or client.create_count != 0
            or client.uploaded_names
        ):
            raise ReleaseContractError(
                "immutable publication recovery performed a mutation or lost evidence"
            )
        client.ruleset["updated_at"] = RELEASE_TAG_RULESET_UPDATED_AT

        def expect_public_shell_rejection(label: str) -> None:
            try:
                verify_main_ci(
                    client,
                    repository,
                    target_sha,
                    FIRST_RELEASE_TAG,
                    allow_exact_published=True,
                )
            except ReleaseContractError:
                pass
            else:
                raise ReleaseContractError(
                    f"publish preflight accepted conflicting public release {label}"
                )
            expect_prepare_rejection(f"conflicting public release {label}")
            expect_publish_rejection(f"conflicting public release {label}")

        reset_public_release()
        assert client.release is not None
        client.release["target_commitish"] = "1" * 40
        expect_public_shell_rejection("target SHA")

        reset_public_release()
        assert client.release is not None
        client.release["name"] = "wrong-title"
        expect_public_shell_rejection("title")

        reset_public_release()
        assert client.release is not None
        client.release["body"] = "wrong notes"
        expect_public_shell_rejection("notes")

        reset_public_release()
        assert client.tag_ref is not None
        client.tag_ref["object"]["sha"] = "2" * 40
        expect_public_shell_rejection("tag ref")

        reset_public_release()
        client.release_read_id_override = release_id + 1
        expect_public_shell_rejection("numeric id")

        reset_public_release()
        assert client.release is not None
        client.release["assets"][0]["digest"] = f"sha256:{'9' * 64}"
        asset_shell = verify_main_ci(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            allow_exact_published=True,
        )
        if asset_shell.get("release_state") != RELEASE_STATE_PUBLISHED:
            raise ReleaseContractError(
                "public release shell did not preserve deferred asset validation"
            )
        expect_prepare_rejection("conflicting public release asset digest")
        expect_publish_rejection("conflicting public release asset digest")

        client.immutable_setting_readable = False
        protection = verify_repository_protection(
            client, repository, allow_unreadable_immutable_setting=True
        )
        if protection["immutable_setting"] != "administration-read-unavailable":
            raise ReleaseContractError("unreadable immutable setting was not disclosed")
        try:
            verify_repository_protection(
                client, repository, allow_unreadable_immutable_setting=False
            )
        except GitHubAPIRequestError:
            pass
        else:
            raise ReleaseContractError("strict protection check accepted unreadable setting")
        client.immutable_setting_readable = True

        client.immutable_setting_enabled = False
        try:
            verify_repository_protection(
                client, repository, allow_unreadable_immutable_setting=True
            )
        except GitHubAPIRequestError as error:
            if error.status != 404:
                raise
        else:
            raise ReleaseContractError("protection check accepted disabled immutable releases")
        client.immutable_setting_enabled = True

        reset_release()
        result = publish_verified_release(
            client,
            repository,
            target_sha,
            FIRST_RELEASE_TAG,
            release_id,
            assets,
            proof,
            proof_run_id,
            proof_run_attempt,
        )
        if result["asset_count"] != len(release_files) or result["immutable"] is not True:
            raise ReleaseContractError("publication self-test lost immutable release evidence")

        cli_asset = assets / expected_package_names(FIRST_RELEASE_TAG)[1]
        original = cli_asset.read_bytes()
        cli_asset.write_bytes(original + b"tampered")
        try:
            verify_verification_proof(
                proof,
                assets,
                target_sha,
                FIRST_RELEASE_TAG,
                repository,
                proof_run_id,
                proof_run_attempt,
            )
        except ReleaseContractError:
            pass
        else:
            raise ReleaseContractError("verification proof accepted a modified release asset")

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
    verify_main.add_argument("--allow-exact-published", action="store_true")
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

    create_gnfs_source = subparsers.add_parser("create-gnfs-source")
    _add_identity_arguments(create_gnfs_source)
    create_gnfs_source.add_argument("--output-directory", type=Path, required=True)
    create_gnfs_source.add_argument("--repository-root", type=Path, default=Path.cwd())

    validate_gnfs_source = subparsers.add_parser("validate-gnfs-source")
    _add_identity_arguments(validate_gnfs_source)
    validate_gnfs_source.add_argument("--archive", type=Path, required=True)
    validate_gnfs_source.add_argument("--repository-root", type=Path, default=Path.cwd())

    fetch_sources = subparsers.add_parser("fetch-sources")
    fetch_sources.add_argument("--output-directory", type=Path, required=True)

    validate_sources = subparsers.add_parser("validate-sources")
    validate_sources.add_argument("--source-directory", type=Path, required=True)

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
    proof.add_argument("--repository", required=True)
    proof.add_argument("--workflow-run-id", type=int, required=True)
    proof.add_argument("--workflow-run-attempt", type=int, required=True)
    proof.add_argument("--server-url", required=True)

    verify_proof = subparsers.add_parser("verify-proof")
    _add_identity_arguments(verify_proof)
    verify_proof.add_argument("--asset-directory", type=Path, required=True)
    verify_proof.add_argument("--proof", type=Path, required=True)
    verify_proof.add_argument("--repository", required=True)
    verify_proof.add_argument("--expected-workflow-run-id", type=int, required=True)
    verify_proof.add_argument("--expected-workflow-run-attempt", type=int, required=True)

    protection = subparsers.add_parser("verify-protection")
    protection.add_argument("--repository", required=True)
    protection.add_argument("--allow-unreadable-immutable-setting", action="store_true")

    prepare_draft = subparsers.add_parser("prepare-draft")
    _add_identity_arguments(prepare_draft)
    prepare_draft.add_argument("--repository", required=True)
    prepare_draft.add_argument("--asset-directory", type=Path, required=True)
    prepare_draft.add_argument("--proof", type=Path, required=True)
    prepare_draft.add_argument("--verification-workflow-run-id", type=int, required=True)
    prepare_draft.add_argument(
        "--verification-workflow-run-attempt", type=int, required=True
    )
    prepare_draft.add_argument("--github-output", type=Path)

    publish_release = subparsers.add_parser("publish-release")
    _add_identity_arguments(publish_release)
    publish_release.add_argument("--repository", required=True)
    publish_release.add_argument("--expected-release-id", type=int, required=True)
    publish_release.add_argument("--asset-directory", type=Path, required=True)
    publish_release.add_argument("--proof", type=Path, required=True)
    publish_release.add_argument("--verification-workflow-run-id", type=int, required=True)
    publish_release.add_argument(
        "--verification-workflow-run-attempt", type=int, required=True
    )

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
                allow_exact_published=arguments.allow_exact_published,
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
            workbench_run_id = evidence.get("workbench_run_id")
            if type(workbench_run_id) is int:
                _write_github_output(
                    arguments.github_output, "workbench_run_id", workbench_run_id
                )
            _write_github_output(
                arguments.github_output,
                "release_state",
                evidence.get("release_state", "unpublished"),
            )
        elif arguments.command == "find-verification":
            run_id, run_attempt = find_verification_run(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
            )
            print(run_id)
            _write_github_output(arguments.github_output, "verification_run_id", run_id)
            _write_github_output(
                arguments.github_output, "verification_run_attempt", run_attempt
            )
        elif arguments.command == "validate-workbench":
            validate_workbench_artifact(
                arguments.artifact_directory, arguments.target_sha, arguments.release_tag
            )
        elif arguments.command == "validate-cli-archive":
            validate_cli_archive(
                arguments.archive, arguments.platform, arguments.release_tag
            )
        elif arguments.command == "create-gnfs-source":
            create_gnfs_source_archive(
                arguments.output_directory,
                arguments.repository_root,
                arguments.target_sha,
                arguments.release_tag,
            )
        elif arguments.command == "validate-gnfs-source":
            validate_gnfs_source_archive(
                arguments.archive,
                arguments.repository_root,
                arguments.target_sha,
                arguments.release_tag,
            )
        elif arguments.command == "fetch-sources":
            fetch_dependency_source_archives(arguments.output_directory)
        elif arguments.command == "validate-sources":
            validate_dependency_source_archives(arguments.source_directory)
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
                arguments.repository,
                arguments.workflow_run_id,
                arguments.workflow_run_attempt,
                arguments.server_url,
            )
        elif arguments.command == "verify-proof":
            verify_verification_proof(
                arguments.proof,
                arguments.asset_directory,
                arguments.target_sha,
                arguments.release_tag,
                arguments.repository,
                arguments.expected_workflow_run_id,
                arguments.expected_workflow_run_attempt,
            )
        elif arguments.command == "verify-protection":
            evidence = verify_repository_protection(
                _client_from_environment(),
                arguments.repository,
                allow_unreadable_immutable_setting=(
                    arguments.allow_unreadable_immutable_setting
                ),
            )
            print(json.dumps(evidence, indent=2, sort_keys=True))
        elif arguments.command == "prepare-draft":
            evidence = prepare_draft_release(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
                arguments.asset_directory,
                arguments.proof,
                arguments.verification_workflow_run_id,
                arguments.verification_workflow_run_attempt,
            )
            print(json.dumps(evidence, indent=2, sort_keys=True))
            _write_github_output(
                arguments.github_output, "release_id", evidence["release_id"]
            )
        elif arguments.command == "publish-release":
            evidence = publish_verified_release(
                _client_from_environment(),
                arguments.repository,
                arguments.target_sha,
                arguments.release_tag,
                arguments.expected_release_id,
                arguments.asset_directory,
                arguments.proof,
                arguments.verification_workflow_run_id,
                arguments.verification_workflow_run_attempt,
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
