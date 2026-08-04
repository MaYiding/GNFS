#!/usr/bin/env python3
"""Fail-closed platform checks for GNFS release executables."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


LINUX_VERSION_CEILINGS = {
    "GLIBC": (2, 31),
    "GLIBCXX": (3, 4, 30),
    "CXXABI": (1, 3, 13),
}
LINUX_ALLOWED_NEEDED = (
    re.compile(r"libc\.so\.\d+"),
    re.compile(r"libdl\.so\.\d+"),
    re.compile(r"libgcc_s\.so\.\d+"),
    re.compile(r"libgmp(?:xx)?\.so\.\d+"),
    re.compile(r"libm\.so\.\d+"),
    re.compile(r"libntl\.so\.\d+"),
    re.compile(r"libpthread\.so\.\d+"),
    re.compile(r"librt\.so\.\d+"),
    re.compile(r"libstdc\+\+\.so\.\d+"),
)


class BinaryContractError(RuntimeError):
    """Raised when an executable exceeds its advertised platform baseline."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _format_version(version: tuple[int, ...]) -> str:
    return ".".join(map(str, version))


def _run(command: list[str]) -> str:
    try:
        completed = subprocess.run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        raise BinaryContractError(f"unable to execute {command[0]}: {error}") from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()[:1000]
        raise BinaryContractError(
            f"{' '.join(command)} failed with status {completed.returncode}: {detail}"
        )
    if not completed.stdout.strip():
        raise BinaryContractError(f"{' '.join(command)} produced no inspection output")
    return completed.stdout


def _numeric_version(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", value):
        raise BinaryContractError(f"non-numeric ABI version token: {value}")
    return tuple(int(component) for component in value.split("."))


def _parse_linux_versions(output: str) -> dict[str, tuple[int, ...]]:
    tokens = set(re.findall(r"\b(?:GLIBCXX|GLIBC|CXXABI)_[A-Za-z0-9_.]+\b", output))
    observed: dict[str, list[tuple[int, ...]]] = {name: [] for name in LINUX_VERSION_CEILINGS}
    for token in tokens:
        family, value = token.split("_", 1)
        observed[family].append(_numeric_version(value))
    missing = [family for family, versions in observed.items() if not versions]
    if missing:
        raise BinaryContractError(f"readelf output lacks required ABI families: {missing}")
    return {family: max(versions) for family, versions in observed.items()}


def _parse_needed(output: str) -> list[str]:
    libraries = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", output)
    if not libraries:
        raise BinaryContractError("readelf found no dynamic dependencies")
    if len(libraries) != len(set(libraries)):
        raise BinaryContractError("readelf reported duplicate dynamic dependencies")
    unexpected = [
        library
        for library in libraries
        if not any(pattern.fullmatch(library) for pattern in LINUX_ALLOWED_NEEDED)
    ]
    if unexpected:
        raise BinaryContractError(f"unexpected Linux dynamic dependencies: {unexpected}")
    return sorted(libraries)


def _assert_linux_version_ceilings(maxima: dict[str, tuple[int, ...]]) -> None:
    violations = {
        family: version
        for family, version in maxima.items()
        if version > LINUX_VERSION_CEILINGS[family]
    }
    if violations:
        detail = ", ".join(
            f"{family}_{'.'.join(map(str, version))} > "
            f"{family}_{'.'.join(map(str, LINUX_VERSION_CEILINGS[family]))}"
            for family, version in sorted(violations.items())
        )
        raise BinaryContractError(f"Linux executable exceeds its advertised ABI baseline: {detail}")


def _write_linux_metadata_and_readme(
    executable: Path,
    maxima: dict[str, tuple[int, ...]],
    needed: list[str],
    metadata_output: Path,
    readme_output: Path,
    release_tag: str,
    target_sha: str,
) -> None:
    if release_tag != "v0.1.0" or not re.fullmatch(r"[0-9a-f]{40}", target_sha):
        raise BinaryContractError("Linux metadata identity is not the locked release tag and SHA")
    if metadata_output.exists() or metadata_output.is_symlink():
        raise BinaryContractError(f"refusing to overwrite Linux metadata: {metadata_output}")
    if readme_output.exists() or readme_output.is_symlink():
        raise BinaryContractError(f"refusing to overwrite Linux README: {readme_output}")
    if metadata_output.parent != executable.parent.parent or readme_output.parent != executable.parent.parent:
        raise BinaryContractError("Linux metadata and README must be written at the package root")
    requirements = {family: _format_version(maxima[family]) for family in LINUX_VERSION_CEILINGS}
    ceilings = {
        family: _format_version(LINUX_VERSION_CEILINGS[family])
        for family in LINUX_VERSION_CEILINGS
    }
    metadata = {
        "abi_ceilings": ceilings,
        "abi_requirements": requirements,
        "architecture": "x86_64",
        "binary_sha256": _sha256(executable),
        "dynamic_dependencies": needed,
        "platform": "linux-x86_64",
        "schema_version": 1,
    }
    with metadata_output.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(metadata, handle, indent=2, sort_keys=True)
        handle.write("\n")
    with readme_output.open("x", encoding="utf-8", newline="\n") as handle:
        handle.write(
            f"GNFS {release_tag}\n"
            f"Built from {target_sha}\n"
            "Linux x86_64.\n"
            "Minimum runtime symbol versions: "
            f"GLIBC_{requirements['GLIBC']}, GLIBCXX_{requirements['GLIBCXX']}, "
            f"CXXABI_{requirements['CXXABI']}.\n"
            "The release ceiling is GLIBC_2.31, GLIBCXX_3.4.30, CXXABI_1.3.13.\n"
            "GMP and NTL dynamic libraries are required from the host and are not bundled.\n"
            "See binary-compatibility.json, LICENSE, and THIRD_PARTY_NOTICES.txt.\n"
        )


def check_linux(
    executable: Path,
    metadata_output: Path | None = None,
    readme_output: Path | None = None,
    release_tag: str | None = None,
    target_sha: str | None = None,
) -> None:
    if not executable.is_file() or executable.is_symlink():
        raise BinaryContractError(f"Linux release executable is not a regular file: {executable}")
    header = _run(["readelf", "--file-header", str(executable)])
    machine_matches = re.findall(r"^\s*Machine:\s*(.+?)\s*$", header, flags=re.MULTILINE)
    if machine_matches != ["Advanced Micro Devices X86-64"]:
        raise BinaryContractError(f"Linux executable is not unambiguously x86_64: {machine_matches}")

    version_info = _run(["readelf", "--version-info", str(executable)])
    maxima = _parse_linux_versions(version_info)
    _assert_linux_version_ceilings(maxima)

    dynamic = _run(["readelf", "--dynamic", str(executable)])
    needed = _parse_needed(dynamic)
    output_arguments = (metadata_output, readme_output, release_tag, target_sha)
    if any(value is not None for value in output_arguments):
        if any(value is None for value in output_arguments):
            raise BinaryContractError(
                "Linux metadata output requires both paths, release tag, and target SHA"
            )
        _write_linux_metadata_and_readme(
            executable,
            maxima,
            needed,
            metadata_output,
            readme_output,
            release_tag,
            target_sha,
        )
    print(
        "Linux release binary contract: PASS "
        f"(x86_64; GLIBC_{'.'.join(map(str, maxima['GLIBC']))}; "
        f"GLIBCXX_{'.'.join(map(str, maxima['GLIBCXX']))}; "
        f"CXXABI_{'.'.join(map(str, maxima['CXXABI']))}; "
        f"needed={','.join(needed)})"
    )


def _parse_macos_minos(
    output: str, tool: str, expected_platform: str = "MACOS"
) -> tuple[int, ...]:
    platforms = re.findall(r"^\s*platform\s+([^\s]+)\s*$", output, flags=re.MULTILINE)
    versions = re.findall(r"^\s*minos\s+([0-9]+(?:\.[0-9]+)+)\s*$", output, flags=re.MULTILINE)
    if platforms != [expected_platform] or len(versions) != 1:
        raise BinaryContractError(
            f"{tool} did not report exactly one MACOS build-version command: "
            f"platforms={platforms}, minos={versions}"
        )
    return _numeric_version(versions[0])


def _parse_macos_dependencies(output: str) -> list[str]:
    lines = output.splitlines()
    if len(lines) < 2 or not lines[0].endswith(":"):
        raise BinaryContractError("otool -L output lacks an executable heading")
    dependencies: list[str] = []
    for line in lines[1:]:
        match = re.fullmatch(r"\s+(\S+) \(compatibility version .+\)", line)
        if not match:
            raise BinaryContractError(f"unrecognized otool -L record: {line}")
        dependencies.append(match.group(1))
    if not dependencies or len(dependencies) != len(set(dependencies)):
        raise BinaryContractError("otool -L returned no dependencies or duplicate paths")
    gmp_pattern = re.compile(r"/opt/homebrew/opt/gmp/lib/libgmp\.[0-9]+\.dylib")
    ntl_pattern = re.compile(r"/opt/homebrew/opt/ntl/lib/libntl\.[0-9]+\.dylib")
    unexpected = [
        dependency
        for dependency in dependencies
        if not dependency.startswith("/System/Library/")
        and not dependency.startswith("/usr/lib/")
        and not gmp_pattern.fullmatch(dependency)
        and not ntl_pattern.fullmatch(dependency)
    ]
    if unexpected:
        raise BinaryContractError(f"unexpected macOS dynamic dependencies: {unexpected}")
    if sum(bool(gmp_pattern.fullmatch(item)) for item in dependencies) != 1 or sum(
        bool(ntl_pattern.fullmatch(item)) for item in dependencies
    ) != 1:
        raise BinaryContractError("macOS release executable lacks exactly one GMP and NTL link")
    return dependencies


def check_macos(executable: Path) -> None:
    if not executable.is_file() or executable.is_symlink():
        raise BinaryContractError(f"macOS release executable is not a regular file: {executable}")
    architectures = _run(["lipo", "-archs", str(executable)]).split()
    if architectures != ["arm64"]:
        raise BinaryContractError(
            f"macOS release executable must contain only arm64; found {architectures}"
        )

    vtool_minos = _parse_macos_minos(
        _run(["vtool", "-show-build", str(executable)]), "vtool"
    )
    otool_minos = _parse_macos_minos(
        _run(["otool", "-l", str(executable)]), "otool", "1"
    )
    required = (13, 0)
    if vtool_minos != required or otool_minos != required:
        raise BinaryContractError(
            "macOS executable deployment target mismatch: "
            f"vtool={vtool_minos}, otool={otool_minos}, expected={required}"
        )
    _parse_macos_dependencies(_run(["otool", "-L", str(executable)]))
    print("macOS release binary contract: PASS (arm64; minos 13.0)")


def self_test() -> None:
    versions = _parse_linux_versions(
        "GLIBC_2.17 GLIBC_2.31 GLIBCXX_3.4.30 CXXABI_1.3 CXXABI_1.3.13"
    )
    if versions != {"GLIBC": (2, 31), "GLIBCXX": (3, 4, 30), "CXXABI": (1, 3, 13)}:
        raise BinaryContractError("Linux ABI parser returned incorrect maxima")
    _assert_linux_version_ceilings(versions)
    too_new = dict(versions)
    too_new["GLIBC"] = (2, 32)
    try:
        _assert_linux_version_ceilings(too_new)
    except BinaryContractError:
        pass
    else:
        raise BinaryContractError("Linux ABI contract accepted a version above its ceiling")
    try:
        _parse_linux_versions("GLIBC_2.32 GLIBCXX_3.4.30 CXXABI_PRIVATE")
    except BinaryContractError:
        pass
    else:
        raise BinaryContractError("Linux ABI parser accepted a nonnumeric ABI token")
    if _parse_needed(
        " 0x1 (NEEDED) Shared library: [libgmp.so.10]\n"
        " 0x1 (NEEDED) Shared library: [libstdc++.so.6]\n"
    ) != ["libgmp.so.10", "libstdc++.so.6"]:
        raise BinaryContractError("Linux dependency parser changed ordering")
    try:
        _parse_needed(" 0x1 (NEEDED) Shared library: [libsurprise.so.1]\n")
    except BinaryContractError:
        pass
    else:
        raise BinaryContractError("Linux dependency parser accepted an unknown library")
    fixture = "Load command 1\n      cmd LC_BUILD_VERSION\n platform MACOS\n    minos 13.0\n"
    if _parse_macos_minos(fixture, "fixture") != (13, 0):
        raise BinaryContractError("macOS deployment-target parser changed")
    dependencies = _parse_macos_dependencies(
        "gnfs:\n"
        "\t/opt/homebrew/opt/gmp/lib/libgmp.10.dylib (compatibility version 1.0.0)\n"
        "\t/opt/homebrew/opt/ntl/lib/libntl.45.dylib (compatibility version 1.0.0)\n"
        "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
    )
    if len(dependencies) != 3:
        raise BinaryContractError("macOS dependency parser changed")
    with tempfile.TemporaryDirectory(prefix="gnfs-linux-metadata-self-test-") as temp_dir:
        root = Path(temp_dir)
        executable = root / "bin" / "gnfs"
        executable.parent.mkdir()
        executable.write_bytes(b"linux executable fixture")
        metadata = root / "binary-compatibility.json"
        readme = root / "README-release.txt"
        _write_linux_metadata_and_readme(
            executable,
            versions,
            ["libc.so.6", "libstdc++.so.6"],
            metadata,
            readme,
            "v0.1.0",
            "1" * 40,
        )
        payload = json.loads(metadata.read_text(encoding="utf-8"))
        if payload["binary_sha256"] != _sha256(executable):
            raise BinaryContractError("Linux compatibility metadata lost the binary digest")
        if not all(family in readme.read_text() for family in LINUX_VERSION_CEILINGS):
            raise BinaryContractError("Linux generated README omits ABI requirements")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("linux", "macos"):
        check = subparsers.add_parser(command)
        check.add_argument("--executable", type=Path, required=True)
        if command == "linux":
            check.add_argument("--metadata-output", type=Path)
            check.add_argument("--readme-output", type=Path)
            check.add_argument("--release-tag")
            check.add_argument("--target-sha")
    subparsers.add_parser("self-test")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "linux":
            check_linux(
                arguments.executable,
                arguments.metadata_output,
                arguments.readme_output,
                arguments.release_tag,
                arguments.target_sha,
            )
        elif arguments.command == "macos":
            check_macos(arguments.executable)
        else:
            self_test()
            print("release binary contract self-test: PASS")
    except (BinaryContractError, OSError) as error:
        print(f"release binary contract error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
