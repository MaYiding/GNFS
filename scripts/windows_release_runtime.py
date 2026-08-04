#!/usr/bin/env python3
"""Bundle and describe every non-system runtime DLL used by the Windows CLI."""

from __future__ import annotations

import argparse
import hashlib
from http.client import IncompleteRead
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import ssl
import subprocess
import sys
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen

from windows_runtime_contract import (
    CONTRACT_PATH,
    InstallPackage,
    RuntimePackage,
    WindowsRuntimeContractError,
    load_contract,
)


PACKAGE_PATTERN = re.compile(r"^[A-Za-z0-9@+_.-]+$")
VERSION_PATTERN = re.compile(r"^[^\s]+$")
MAX_PINNED_PACKAGE_BYTES = 128 * 1024 * 1024


class RuntimeContractError(RuntimeError):
    """Raised when Windows dependency or license discovery is incomplete."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _run(command: list[str]) -> str:
    try:
        completed = subprocess.run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        raise RuntimeContractError(f"unable to execute {command[0]}: {error}") from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()[:1000]
        raise RuntimeContractError(
            f"{' '.join(command)} failed with status {completed.returncode}: {detail}"
        )
    if not completed.stdout.strip():
        raise RuntimeContractError(f"{' '.join(command)} produced no output")
    return completed.stdout


def parse_ldd(output: str) -> list[PurePosixPath]:
    if re.search(r"\bnot found\b", output, flags=re.IGNORECASE):
        raise RuntimeContractError("ldd reported an unresolved runtime dependency")
    bundled: set[PurePosixPath] = set()
    observed = 0
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = re.fullmatch(r"\S+\s+=>\s+(\S+)\s+\(0x[0-9A-Fa-f]+\)", line)
        if not match:
            raise RuntimeContractError(f"unrecognized ldd record: {line}")
        observed += 1
        resolved_text = match.group(1).replace("\\", "/")
        lowered = resolved_text.lower()
        if lowered.startswith("/ucrt64/bin/"):
            path = PurePosixPath(resolved_text)
            if path.parent.as_posix().lower() != "/ucrt64/bin" or path.suffix.lower() != ".dll":
                raise RuntimeContractError(f"invalid UCRT64 dependency path: {resolved_text}")
            bundled.add(path)
            continue
        if re.fullmatch(r"/[a-z]/windows/(?:system32|syswow64)/[^/]+\.dll", lowered):
            continue
        raise RuntimeContractError(f"dependency is neither UCRT64 nor Windows system: {resolved_text}")
    if observed == 0:
        raise RuntimeContractError("ldd reported no runtime dependencies")
    if not bundled:
        raise RuntimeContractError("ldd resolved no UCRT64 DLLs")
    return sorted(bundled, key=lambda path: path.as_posix().lower())


def _native_path(msys_path: PurePosixPath) -> Path:
    converted = _run(["cygpath", "-w", msys_path.as_posix()]).strip()
    if "\n" in converted or not converted:
        raise RuntimeContractError(f"cygpath returned an invalid path for {msys_path}")
    return Path(converted)


def _package_identity(msys_path: PurePosixPath) -> tuple[str, str]:
    owners = _run(["pacman", "-Qoq", msys_path.as_posix()]).splitlines()
    if len(owners) != 1 or not PACKAGE_PATTERN.fullmatch(owners[0]):
        raise RuntimeContractError(f"DLL must have exactly one valid pacman owner: {msys_path}")
    package = owners[0]
    identity = _run(["pacman", "-Q", package]).split()
    if len(identity) != 2 or identity[0] != package or not VERSION_PATTERN.fullmatch(identity[1]):
        raise RuntimeContractError(f"invalid pacman identity for {package}: {identity}")
    return package, identity[1]


def _package_license_files(package: str) -> list[Path]:
    records = _run(["pacman", "-Ql", package]).splitlines()
    result: list[Path] = []
    for record in records:
        fields = record.split(maxsplit=1)
        if len(fields) != 2 or fields[0] != package:
            raise RuntimeContractError(f"invalid pacman file record for {package}: {record}")
        msys_source = PurePosixPath(fields[1])
        normalized = msys_source.as_posix()
        if not (
            normalized.startswith("/ucrt64/share/licenses/")
            or normalized.startswith("/usr/share/licenses/")
        ):
            continue
        source = _native_path(msys_source)
        if source.is_file() and not source.is_symlink():
            result.append(source)
    result = sorted(set(result), key=lambda path: path.as_posix())
    return result


def _copy_package_licenses(package: str, sources: list[Path], root: Path) -> list[str]:
    destination_directory = root / "licenses" / package
    destination_directory.mkdir(parents=True, exist_ok=True)
    records: list[str] = []
    used_names: set[str] = set()
    for source in sources:
        if source.name in used_names:
            raise RuntimeContractError(
                f"license basename collision for pacman package {package}: {source.name}"
            )
        used_names.add(source.name)
        destination = destination_directory / source.name
        if destination.exists() or destination.is_symlink():
            raise RuntimeContractError(f"refusing to overwrite packaged license: {destination}")
        shutil.copyfile(source, destination)
        records.append(destination.relative_to(root).as_posix())
    return records


def _copy_fallback_licenses(
    package: RuntimePackage, root: Path, used_names: set[str]
) -> list[str]:
    repository_root = Path(__file__).resolve().parents[1]
    destination_directory = root / "licenses" / package.name
    records: list[str] = []
    for fallback in package.fallback_licenses:
        source = (repository_root / fallback.path).resolve()
        try:
            source.relative_to(repository_root)
        except ValueError as error:
            raise RuntimeContractError(
                f"fallback license escaped the repository: {fallback.path}"
            ) from error
        if not source.is_file() or source.is_symlink() or _sha256(source) != fallback.sha256:
            raise RuntimeContractError(
                f"fallback license is missing or changed for {package.name}: {fallback.path}"
            )
        if fallback.archive_name in used_names:
            raise RuntimeContractError(
                f"fallback license name collides for {package.name}: {fallback.archive_name}"
            )
        used_names.add(fallback.archive_name)
        destination_directory.mkdir(parents=True, exist_ok=True)
        destination = destination_directory / fallback.archive_name
        if destination.exists() or destination.is_symlink():
            raise RuntimeContractError(f"refusing to overwrite fallback license: {destination}")
        shutil.copyfile(source, destination)
        records.append(destination.relative_to(root).as_posix())
    return records


def _write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")


def _download_pinned_package(package: InstallPackage, directory: Path) -> Path:
    request = Request(
        package.url,
        headers={
            "Accept": "application/octet-stream",
            "User-Agent": "gnfs-windows-runtime-pin/1",
        },
    )
    partial_path = directory / f".{package.archive}.partial"
    destination = directory / package.archive
    ssl_context = ssl.create_default_context()
    last_error: Exception | None = None
    for attempt in range(1, 4):
        digest = hashlib.sha256()
        size = 0
        try:
            with urlopen(request, timeout=120, context=ssl_context) as response:
                final_url = response.geturl()
                if not isinstance(final_url, str) or not final_url.startswith("https://"):
                    raise RuntimeContractError(
                        f"pinned package download left HTTPS for {package.name}: {final_url}"
                    )
                length_header = response.headers.get("Content-Length")
                expected_size = int(length_header) if length_header is not None else None
                if expected_size is not None and (
                    expected_size <= 0 or expected_size > MAX_PINNED_PACKAGE_BYTES
                ):
                    raise RuntimeContractError(
                        f"pinned package has invalid Content-Length: {package.name}"
                    )
                with partial_path.open("wb") as handle:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        size += len(chunk)
                        if size > MAX_PINNED_PACKAGE_BYTES:
                            raise RuntimeContractError(
                                f"pinned package exceeds size cap: {package.name}"
                            )
                        digest.update(chunk)
                        handle.write(chunk)
                if expected_size is not None and size != expected_size:
                    raise RuntimeContractError(
                        f"pinned package download was truncated: {package.name}"
                    )
                if digest.hexdigest() != package.sha256:
                    raise RuntimeContractError(
                        f"pinned package digest mismatch: {package.name}"
                    )
        except (IncompleteRead, OSError, RuntimeContractError, URLError, ValueError) as error:
            last_error = error
            partial_path.unlink(missing_ok=True)
            if attempt < 3:
                continue
            raise RuntimeContractError(
                f"pinned package download failed after three attempts for {package.name}: {error}"
            ) from error
        os.replace(partial_path, destination)
        return destination
    raise RuntimeContractError(f"pinned package download failed for {package.name}: {last_error}")


def _validate_installed_packages() -> None:
    for package in load_contract().install_packages:
        identity = _run(["pacman", "-Q", package.name]).split()
        if identity != [package.name, package.version]:
            raise RuntimeContractError(
                f"installed package does not match pinned identity for {package.name}: {identity}"
            )


def _pinned_evidence_value() -> dict[str, Any]:
    contract = load_contract()
    return {
        "packages": [
            {
                "archive": package.archive,
                "name": package.name,
                "sha256": package.sha256,
                "version": package.version,
            }
            for package in contract.install_packages
        ],
        "runtime": contract.runtime,
        "schema_version": 1,
    }


def _validate_pinned_evidence(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeContractError(f"pinned package evidence is unreadable: {error}") from error
    if value != _pinned_evidence_value():
        raise RuntimeContractError("pinned package evidence does not match the release contract")
    return value


def install_pinned_packages(download_directory: Path, evidence_output: Path) -> None:
    download_directory = download_directory.resolve()
    evidence_output = evidence_output.resolve()
    if download_directory.exists() or download_directory.is_symlink():
        raise RuntimeContractError(
            f"refusing to reuse pinned package directory: {download_directory}"
        )
    if evidence_output.exists() or evidence_output.is_symlink():
        raise RuntimeContractError(f"refusing to overwrite pinned evidence: {evidence_output}")
    download_directory.parent.mkdir(parents=True, exist_ok=True)
    download_directory.mkdir()
    archives = [
        _download_pinned_package(package, download_directory)
        for package in load_contract().install_packages
    ]
    posix_archives = [_run(["cygpath", "-u", str(path)]).strip() for path in archives]
    _run(["pacman", "-U", "--noconfirm", *posix_archives])
    _validate_installed_packages()
    evidence_output.parent.mkdir(parents=True, exist_ok=True)
    _write_json_exclusive(evidence_output, _pinned_evidence_value())


def _run_packaged_executable(executable: Path, expected_version: str) -> None:
    system_root = os.environ.get("SystemRoot") or os.environ.get("WINDIR")
    if not system_root:
        raise RuntimeContractError("SystemRoot or WINDIR is required for the isolated launch")
    package_bin = str(executable.parent.resolve())
    system32 = str(Path(system_root) / "System32")
    minimal_path = os.pathsep.join((package_bin, system32, system_root))
    if "/ucrt64/bin" in minimal_path.replace("\\", "/").lower():
        raise RuntimeContractError("isolated launch PATH unexpectedly contains /ucrt64/bin")
    environment = {
        "PATH": minimal_path,
        "SystemRoot": system_root,
        "WINDIR": system_root,
    }
    try:
        completed = subprocess.run(
            [str(executable), "--version"],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )
    except OSError as error:
        raise RuntimeContractError(f"packaged executable failed to start: {error}") from error
    output = completed.stdout.replace("\r\n", "\n").replace("\r", "\n").strip()
    if completed.returncode != 0 or output != expected_version:
        raise RuntimeContractError(
            "packaged executable failed its isolated version probe: "
            f"status={completed.returncode}, stdout={output!r}, stderr={completed.stderr[:500]!r}"
        )


def bundle_runtime(
    package_root: Path,
    executable: Path,
    project_license: Path,
    pinned_package_evidence: Path,
    release_tag: str,
    target_sha: str,
) -> None:
    package_root = package_root.resolve()
    executable = executable.resolve()
    project_license = project_license.resolve()
    pinned_package_evidence = pinned_package_evidence.resolve()
    if executable.parent != package_root / "bin" or not executable.is_file():
        raise RuntimeContractError("Windows executable must be package-root/bin/gnfs.exe")
    if not project_license.is_file() or project_license.is_symlink():
        raise RuntimeContractError("project LICENSE must be a regular file")
    if not pinned_package_evidence.is_file() or pinned_package_evidence.is_symlink():
        raise RuntimeContractError("pinned package evidence must be a regular file")
    if not re.fullmatch(r"[0-9a-f]{40}", target_sha):
        raise RuntimeContractError("target SHA must be canonical lowercase 40-hex")

    _validate_pinned_evidence(pinned_package_evidence)
    _validate_installed_packages()
    contract = load_contract()
    runtime_by_name = {package.name: package for package in contract.runtime_packages}
    source_by_name = {source.name: source for source in contract.source_archives}
    install_by_name = {package.name: package for package in contract.install_packages}
    ldd_output = _run(["ldd", str(executable)])
    runtime_paths = parse_ldd(ldd_output)
    package_cache: dict[str, tuple[str, list[str], RuntimePackage]] = {}
    dependencies: list[dict[str, Any]] = []
    for msys_source in runtime_paths:
        source = _native_path(msys_source)
        if not source.is_file() or source.is_symlink():
            raise RuntimeContractError(f"resolved UCRT64 DLL is not a regular file: {source}")
        package, version = _package_identity(msys_source)
        package_contract = runtime_by_name.get(package)
        if package_contract is None or package_contract.version != version:
            raise RuntimeContractError(
                f"runtime package is outside the exact source closure: {package} {version}"
            )
        expected_dll_digest = package_contract.dll_sha256.get(source.name)
        observed_dll_digest = _sha256(source)
        if expected_dll_digest is None or expected_dll_digest != observed_dll_digest:
            raise RuntimeContractError(
                f"runtime DLL does not match its pinned binary package: {source.name}"
            )
        if package not in package_cache:
            package_licenses = _package_license_files(package)
            license_files = _copy_package_licenses(package, package_licenses, package_root)
            license_files.extend(
                _copy_fallback_licenses(
                    package_contract,
                    package_root,
                    {path.name for path in package_licenses},
                )
            )
            license_files.sort()
            if not license_files:
                raise RuntimeContractError(
                    f"runtime package has neither packaged nor pinned fallback licenses: {package}"
                )
            package_cache[package] = (version, license_files, package_contract)
        elif package_cache[package][0] != version:
            raise RuntimeContractError(f"pacman package version changed during collection: {package}")

        destination = executable.parent / source.name
        if destination.exists() or destination.is_symlink():
            raise RuntimeContractError(f"refusing to overwrite packaged DLL: {destination}")
        shutil.copyfile(source, destination)
        dependencies.append(
            {
                "dll": source.name,
                "license_files": package_cache[package][1],
                "package": package,
                "package_archive": install_by_name[package].archive,
                "package_archive_sha256": install_by_name[package].sha256,
                "package_version": version,
                "sha256": observed_dll_digest,
                "source_archive": package_contract.source_archive,
                "source_archive_sha256": source_by_name[
                    package_contract.source_archive
                ].sha256,
            }
        )

    dependencies.sort(key=lambda item: item["dll"].lower())
    if set(package_cache) != set(runtime_by_name):
        raise RuntimeContractError(
            "resolved runtime package set does not match the exact Windows source closure"
        )
    expected_dlls = {
        dll
        for package_contract in contract.runtime_packages
        for dll in package_contract.dll_sha256
    }
    if {dependency["dll"] for dependency in dependencies} != expected_dlls:
        raise RuntimeContractError(
            "resolved runtime DLL set does not match the exact Windows release closure"
        )
    contract_destination = package_root / CONTRACT_PATH.name
    if contract_destination.exists() or contract_destination.is_symlink():
        raise RuntimeContractError(
            f"refusing to overwrite packaged runtime contract: {contract_destination}"
        )
    shutil.copyfile(CONTRACT_PATH, contract_destination)
    manifest = {
        "contract_file": CONTRACT_PATH.name,
        "contract_sha256": _sha256(contract_destination),
        "dependencies": dependencies,
        "runtime": contract.runtime,
        "schema_version": 2,
    }
    _write_json_exclusive(package_root / "runtime-dependencies.json", manifest)

    project_destination = package_root / "LICENSE"
    if project_destination.exists() or project_destination.is_symlink():
        raise RuntimeContractError(f"refusing to overwrite project license: {project_destination}")
    shutil.copyfile(project_license, project_destination)
    notice_lines = [
        "THIRD-PARTY NOTICES",
        "",
        "This Windows archive bundles the UCRT64 DLLs listed below.",
        "Their binary package bytes, DLL hashes, licenses, and corresponding source archives",
        "are fixed in runtime-dependencies.json and windows-ucrt64-runtime.json.",
        "The named MSYS2 source-only archives are separate assets in the same GitHub Release.",
        "",
    ]
    for package in sorted(package_cache):
        version, licenses, package_contract = package_cache[package]
        notice_lines.append(f"{package} {version}")
        if package == "mingw-w64-ucrt-x86_64-gmp":
            notice_lines.append("  license selection: GNU GPL version 2")
        notice_lines.append(f"  corresponding source: {package_contract.source_archive}")
        notice_lines.extend(f"  {license_path}" for license_path in licenses)
    (package_root / "THIRD_PARTY_NOTICES.txt").write_text(
        "\n".join(notice_lines) + "\n", encoding="utf-8", newline="\n"
    )
    (package_root / "README-release.txt").write_text(
        f"GNFS {release_tag}\n"
        f"Built from {target_sha}\n"
        "\n"
        "This archive contains the Windows x86_64 GNFS CLI, headers, static libraries, "
        "and its resolved MSYS2 UCRT64 runtime DLLs.\n"
        "See LICENSE, THIRD_PARTY_NOTICES.txt, runtime-dependencies.json, "
        "windows-ucrt64-runtime.json, and licenses/. The exact MSYS2 source-only archives "
        "named by the manifest are published beside this ZIP.\n",
        encoding="utf-8",
        newline="\n",
    )
    _run_packaged_executable(executable, f"GNFS {release_tag}")


def self_test() -> None:
    fixture = (
        "libgmp-10.dll => /ucrt64/bin/libgmp-10.dll (0x0001)\n"
        "KERNEL32.DLL => /c/WINDOWS/System32/KERNEL32.DLL (0x0002)\n"
    )
    if parse_ldd(fixture) != [PurePosixPath("/ucrt64/bin/libgmp-10.dll")]:
        raise RuntimeContractError("ldd parser returned an unexpected dependency set")
    for bad in (
        "libgmp-10.dll => not found (0x0001)\n",
        "libbad.dll => /mingw64/bin/libbad.dll (0x0001)\n",
        "unstructured output\n",
    ):
        try:
            parse_ldd(bad)
        except RuntimeContractError:
            pass
        else:
            raise RuntimeContractError(f"ldd parser accepted unsafe fixture: {bad!r}")
    contract = load_contract()
    gmp = next(
        package
        for package in contract.runtime_packages
        if package.name == "mingw-w64-ucrt-x86_64-gmp"
    )
    if len(gmp.fallback_licenses) != 1:
        raise RuntimeContractError("GMP runtime contract lost its pinned fallback licenses")
    import tempfile

    with tempfile.TemporaryDirectory(prefix="gnfs-windows-runtime-evidence-") as directory:
        evidence = Path(directory) / "pinned.json"
        _write_json_exclusive(evidence, _pinned_evidence_value())
        _validate_pinned_evidence(evidence)
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["packages"][0]["sha256"] = "0" * 64
        evidence.write_text(json.dumps(value), encoding="utf-8")
        try:
            _validate_pinned_evidence(evidence)
        except RuntimeContractError:
            pass
        else:
            raise RuntimeContractError("pinned evidence accepted a changed binary digest")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    bundle = subparsers.add_parser("bundle")
    bundle.add_argument("--package-root", type=Path, required=True)
    bundle.add_argument("--executable", type=Path, required=True)
    bundle.add_argument("--pinned-package-evidence", type=Path, required=True)
    bundle.add_argument("--project-license", type=Path, required=True)
    bundle.add_argument("--release-tag", required=True)
    bundle.add_argument("--target-sha", required=True)
    install = subparsers.add_parser("install-pinned")
    install.add_argument("--download-directory", type=Path, required=True)
    install.add_argument("--evidence-output", type=Path, required=True)
    subparsers.add_parser("self-test")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "bundle":
            bundle_runtime(
                arguments.package_root,
                arguments.executable,
                arguments.project_license,
                arguments.pinned_package_evidence,
                arguments.release_tag,
                arguments.target_sha,
            )
        elif arguments.command == "install-pinned":
            install_pinned_packages(
                arguments.download_directory,
                arguments.evidence_output,
            )
        else:
            self_test()
            print("Windows release runtime self-test: PASS")
    except (
        RuntimeContractError,
        WindowsRuntimeContractError,
        OSError,
        json.JSONDecodeError,
    ) as error:
        print(f"Windows release runtime error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
