#!/usr/bin/env python3
"""Bundle and describe every non-system runtime DLL used by the Windows CLI."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
from typing import Any


PACKAGE_PATTERN = re.compile(r"^[A-Za-z0-9@+_.-]+$")
VERSION_PATTERN = re.compile(r"^[^\s]+$")


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
    if not result:
        raise RuntimeContractError(
            f"pacman package {package} has no regular license file under an approved license root"
        )
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


def _write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")


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
    release_tag: str,
    target_sha: str,
) -> None:
    package_root = package_root.resolve()
    executable = executable.resolve()
    project_license = project_license.resolve()
    if executable.parent != package_root / "bin" or not executable.is_file():
        raise RuntimeContractError("Windows executable must be package-root/bin/gnfs.exe")
    if not project_license.is_file() or project_license.is_symlink():
        raise RuntimeContractError("project LICENSE must be a regular file")
    if not re.fullmatch(r"[0-9a-f]{40}", target_sha):
        raise RuntimeContractError("target SHA must be canonical lowercase 40-hex")

    ldd_output = _run(["ldd", str(executable)])
    runtime_paths = parse_ldd(ldd_output)
    package_cache: dict[str, tuple[str, list[str]]] = {}
    dependencies: list[dict[str, Any]] = []
    for msys_source in runtime_paths:
        source = _native_path(msys_source)
        if not source.is_file() or source.is_symlink():
            raise RuntimeContractError(f"resolved UCRT64 DLL is not a regular file: {source}")
        package, version = _package_identity(msys_source)
        if package not in package_cache:
            license_files = _copy_package_licenses(
                package, _package_license_files(package), package_root
            )
            package_cache[package] = (version, license_files)
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
                "package_version": version,
                "sha256": _sha256(destination),
            }
        )

    dependencies.sort(key=lambda item: item["dll"].lower())
    manifest = {
        "dependencies": dependencies,
        "runtime": "MSYS2 UCRT64",
        "schema_version": 1,
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
        "Their package versions and copied license files are fixed in runtime-dependencies.json.",
        "",
    ]
    for package in sorted(package_cache):
        version, licenses = package_cache[package]
        notice_lines.append(f"{package} {version}")
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
        "See LICENSE, THIRD_PARTY_NOTICES.txt, runtime-dependencies.json, and licenses/.\n",
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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    bundle = subparsers.add_parser("bundle")
    bundle.add_argument("--package-root", type=Path, required=True)
    bundle.add_argument("--executable", type=Path, required=True)
    bundle.add_argument("--project-license", type=Path, required=True)
    bundle.add_argument("--release-tag", required=True)
    bundle.add_argument("--target-sha", required=True)
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
                arguments.release_tag,
                arguments.target_sha,
            )
        else:
            self_test()
            print("Windows release runtime self-test: PASS")
    except (RuntimeContractError, OSError, json.JSONDecodeError) as error:
        print(f"Windows release runtime error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
