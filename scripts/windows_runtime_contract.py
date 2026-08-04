#!/usr/bin/env python3
"""Load the immutable MSYS2 UCRT64 binary/source closure for Windows releases."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import re
import tempfile
from typing import Any
from urllib.parse import urlparse


CONTRACT_PATH = Path(__file__).with_name("windows-ucrt64-runtime.json")
EXPECTED_INSTALL_PACKAGES = (
    "mingw-w64-ucrt-x86_64-gcc",
    "mingw-w64-ucrt-x86_64-gcc-libs",
    "mingw-w64-ucrt-x86_64-gmp",
    "mingw-w64-ucrt-x86_64-libwinpthread",
    "mingw-w64-ucrt-x86_64-winpthreads",
)
EXPECTED_RUNTIME_PACKAGES = (
    "mingw-w64-ucrt-x86_64-gcc-libs",
    "mingw-w64-ucrt-x86_64-gmp",
    "mingw-w64-ucrt-x86_64-libwinpthread",
)
EXPECTED_RUNTIME_DLLS = {
    "mingw-w64-ucrt-x86_64-gcc-libs": (
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
    ),
    "mingw-w64-ucrt-x86_64-gmp": ("libgmp-10.dll",),
    "mingw-w64-ucrt-x86_64-libwinpthread": ("libwinpthread-1.dll",),
}
EXPECTED_FALLBACK_LICENSES = {
    "mingw-w64-ucrt-x86_64-gcc-libs": (),
    "mingw-w64-ucrt-x86_64-gmp": (
        (
            "COPYINGv2",
            "apps/macos/GNFSWorkbench/GNFSWorkbench/Resources/Licenses/GMP-COPYING.txt",
            "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643",
        ),
    ),
    "mingw-w64-ucrt-x86_64-libwinpthread": (),
}
PACKAGE_PATTERN = re.compile(r"^[A-Za-z0-9@+_.-]+$")
VERSION_PATTERN = re.compile(r"^[^\s]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class WindowsRuntimeContractError(RuntimeError):
    """Raised when the checked-in Windows runtime closure is malformed."""


@dataclass(frozen=True)
class InstallPackage:
    archive: str
    name: str
    sha256: str
    url: str
    version: str


@dataclass(frozen=True)
class FallbackLicense:
    archive_name: str
    path: str
    sha256: str


@dataclass(frozen=True)
class RuntimePackage:
    dll_sha256: dict[str, str]
    fallback_licenses: tuple[FallbackLicense, ...]
    name: str
    source_archive: str
    version: str


@dataclass(frozen=True)
class SourceArchive:
    name: str
    required_paths: tuple[str, ...]
    root: str
    sha256: str
    url: str


@dataclass(frozen=True)
class WindowsRuntimeContract:
    install_packages: tuple[InstallPackage, ...]
    runtime: str
    runtime_packages: tuple[RuntimePackage, ...]
    schema_version: int
    source_archives: tuple[SourceArchive, ...]


def _require_keys(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        raise WindowsRuntimeContractError(f"{label} has missing or unknown fields")
    return value


def _safe_basename(value: Any, suffix: str, label: str) -> str:
    if (
        not isinstance(value, str)
        or PurePosixPath(value).name != value
        or not value.endswith(suffix)
        or not re.fullmatch(r"[A-Za-z0-9._+-]+", value)
    ):
        raise WindowsRuntimeContractError(f"{label} is invalid")
    return value


def _validate_url(value: Any, expected_directory: str, expected_name: str) -> str:
    if not isinstance(value, str):
        raise WindowsRuntimeContractError(f"URL for {expected_name} is invalid")
    parsed = urlparse(value)
    expected_path = f"{expected_directory}/{expected_name}"
    if (
        parsed.scheme != "https"
        or parsed.netloc != "repo.msys2.org"
        or parsed.path != expected_path
        or parsed.params
        or parsed.query
        or parsed.fragment
    ):
        raise WindowsRuntimeContractError(f"URL for {expected_name} is not canonical")
    return value


def _validate_relative_path(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise WindowsRuntimeContractError(f"{label} is invalid")
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts or "\\" in value:
        raise WindowsRuntimeContractError(f"{label} is unsafe")
    return value


def load_contract(path: Path = CONTRACT_PATH) -> WindowsRuntimeContract:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise WindowsRuntimeContractError(f"unable to load {path.name}: {error}") from error
    root = _require_keys(
        raw,
        {"install_packages", "runtime", "runtime_packages", "schema_version", "source_archives"},
        "Windows runtime contract",
    )
    if root["schema_version"] != 1 or root["runtime"] != "MSYS2 UCRT64":
        raise WindowsRuntimeContractError("Windows runtime contract identity is invalid")

    install_packages: list[InstallPackage] = []
    if not isinstance(root["install_packages"], list):
        raise WindowsRuntimeContractError("install_packages must be a list")
    for index, value in enumerate(root["install_packages"]):
        record = _require_keys(
            value,
            {"archive", "name", "sha256", "url", "version"},
            f"install package {index}",
        )
        name = record["name"]
        version = record["version"]
        digest = record["sha256"]
        if not isinstance(name, str) or not PACKAGE_PATTERN.fullmatch(name):
            raise WindowsRuntimeContractError(f"install package {index} has an invalid name")
        if not isinstance(version, str) or not VERSION_PATTERN.fullmatch(version):
            raise WindowsRuntimeContractError(f"install package {name} has an invalid version")
        archive = _safe_basename(record["archive"], ".pkg.tar.zst", f"archive for {name}")
        if archive != f"{name}-{version}-any.pkg.tar.zst":
            raise WindowsRuntimeContractError(f"binary archive identity diverged for {name}")
        if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
            raise WindowsRuntimeContractError(f"binary archive digest is invalid for {name}")
        install_packages.append(
            InstallPackage(
                archive=archive,
                name=name,
                sha256=digest,
                url=_validate_url(record["url"], "/mingw/ucrt64", archive),
                version=version,
            )
        )
    install_names = tuple(package.name for package in install_packages)
    if install_names != EXPECTED_INSTALL_PACKAGES:
        raise WindowsRuntimeContractError("pinned install package set is not exact and sorted")
    install_by_name = {package.name: package for package in install_packages}
    if (
        install_by_name["mingw-w64-ucrt-x86_64-gcc"].version
        != install_by_name["mingw-w64-ucrt-x86_64-gcc-libs"].version
        or install_by_name["mingw-w64-ucrt-x86_64-libwinpthread"].version
        != install_by_name["mingw-w64-ucrt-x86_64-winpthreads"].version
    ):
        raise WindowsRuntimeContractError("paired compiler/runtime package versions diverged")
    expected_source_by_runtime = {
        "mingw-w64-ucrt-x86_64-gcc-libs": (
            "mingw-w64-gcc-"
            f"{install_by_name['mingw-w64-ucrt-x86_64-gcc'].version}.src.tar.zst"
        ),
        "mingw-w64-ucrt-x86_64-gmp": (
            "mingw-w64-gmp-"
            f"{install_by_name['mingw-w64-ucrt-x86_64-gmp'].version}.src.tar.zst"
        ),
        "mingw-w64-ucrt-x86_64-libwinpthread": (
            "mingw-w64-winpthreads-"
            f"{install_by_name['mingw-w64-ucrt-x86_64-winpthreads'].version}.src.tar.zst"
        ),
    }

    sources: list[SourceArchive] = []
    if not isinstance(root["source_archives"], list):
        raise WindowsRuntimeContractError("source_archives must be a list")
    for index, value in enumerate(root["source_archives"]):
        record = _require_keys(
            value,
            {"name", "required_paths", "root", "sha256", "url"},
            f"source archive {index}",
        )
        name = _safe_basename(record["name"], ".src.tar.zst", f"source archive {index}")
        source_root = record["root"]
        digest = record["sha256"]
        required_paths = record["required_paths"]
        if not isinstance(source_root, str) or not PACKAGE_PATTERN.fullmatch(source_root):
            raise WindowsRuntimeContractError(f"source archive {name} has an invalid root")
        if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
            raise WindowsRuntimeContractError(f"source archive digest is invalid for {name}")
        if not isinstance(required_paths, list) or not required_paths:
            raise WindowsRuntimeContractError(f"source archive {name} has no required paths")
        validated_paths = tuple(
            _validate_relative_path(item, f"required path in {name}") for item in required_paths
        )
        if validated_paths != tuple(sorted(set(validated_paths))):
            raise WindowsRuntimeContractError(f"required paths for {name} are not unique and sorted")
        sources.append(
            SourceArchive(
                name=name,
                required_paths=validated_paths,
                root=source_root,
                sha256=digest,
                url=_validate_url(record["url"], "/mingw/sources", name),
            )
        )
    source_names = tuple(source.name for source in sources)
    expected_source_names = tuple(sorted(expected_source_by_runtime.values()))
    if source_names != expected_source_names:
        raise WindowsRuntimeContractError("source archive set is not exact and sorted")

    runtime_packages: list[RuntimePackage] = []
    if not isinstance(root["runtime_packages"], list):
        raise WindowsRuntimeContractError("runtime_packages must be a list")
    source_by_name = {source.name: source for source in sources}
    for index, value in enumerate(root["runtime_packages"]):
        record = _require_keys(
            value,
            {"dll_sha256", "fallback_licenses", "name", "source_archive", "version"},
            f"runtime package {index}",
        )
        name = record["name"]
        version = record["version"]
        source_archive = record["source_archive"]
        if not isinstance(name, str) or not PACKAGE_PATTERN.fullmatch(name):
            raise WindowsRuntimeContractError(f"runtime package {index} has an invalid name")
        if not isinstance(version, str) or not VERSION_PATTERN.fullmatch(version):
            raise WindowsRuntimeContractError(f"runtime package {name} has an invalid version")
        if source_archive not in source_by_name:
            raise WindowsRuntimeContractError(f"runtime package {name} has unknown source")
        if source_archive != expected_source_by_runtime.get(name):
            raise WindowsRuntimeContractError(
                f"runtime package {name} does not map to its exact source package"
            )
        install = install_by_name.get(name)
        if install is None or install.version != version:
            raise WindowsRuntimeContractError(f"runtime package {name} is not installed exactly")
        dlls = record["dll_sha256"]
        if not isinstance(dlls, dict) or not dlls:
            raise WindowsRuntimeContractError(f"runtime package {name} has no DLL contract")
        dll_sha256: dict[str, str] = {}
        for dll, digest in sorted(dlls.items(), key=lambda item: item[0].lower()):
            if (
                not isinstance(dll, str)
                or PurePosixPath(dll).name != dll
                or not dll.lower().endswith(".dll")
                or not isinstance(digest, str)
                or not SHA256_PATTERN.fullmatch(digest)
            ):
                raise WindowsRuntimeContractError(f"runtime DLL contract is invalid for {name}")
            dll_sha256[dll] = digest
        if list(dlls) != sorted(set(dlls), key=str.lower):
            raise WindowsRuntimeContractError(f"runtime DLLs for {name} are not unique and sorted")
        if tuple(dll_sha256) != EXPECTED_RUNTIME_DLLS.get(name):
            raise WindowsRuntimeContractError(
                f"runtime DLL set is outside the fixed first-release closure for {name}"
            )

        fallback_licenses: list[FallbackLicense] = []
        if not isinstance(record["fallback_licenses"], list):
            raise WindowsRuntimeContractError(f"fallback licenses for {name} must be a list")
        for license_index, license_value in enumerate(record["fallback_licenses"]):
            license_record = _require_keys(
                license_value,
                {"archive_name", "path", "sha256"},
                f"fallback license {license_index} for {name}",
            )
            archive_name = _safe_basename(
                license_record["archive_name"], "", f"fallback license name for {name}"
            )
            license_path = _validate_relative_path(
                license_record["path"], f"fallback license path for {name}"
            )
            license_digest = license_record["sha256"]
            if not isinstance(license_digest, str) or not SHA256_PATTERN.fullmatch(
                license_digest
            ):
                raise WindowsRuntimeContractError(f"fallback license digest is invalid for {name}")
            fallback_licenses.append(
                FallbackLicense(archive_name, license_path, license_digest)
            )
        if tuple(item.archive_name for item in fallback_licenses) != tuple(
            sorted({item.archive_name for item in fallback_licenses})
        ):
            raise WindowsRuntimeContractError(
                f"fallback licenses for {name} are duplicate or unsorted"
            )
        observed_fallbacks = tuple(
            (item.archive_name, item.path, item.sha256) for item in fallback_licenses
        )
        if observed_fallbacks != EXPECTED_FALLBACK_LICENSES.get(name):
            raise WindowsRuntimeContractError(
                f"fallback licenses are outside the fixed first-release contract for {name}"
            )
        runtime_packages.append(
            RuntimePackage(
                dll_sha256=dll_sha256,
                fallback_licenses=tuple(fallback_licenses),
                name=name,
                source_archive=source_archive,
                version=version,
            )
        )
    runtime_names = tuple(package.name for package in runtime_packages)
    if runtime_names != EXPECTED_RUNTIME_PACKAGES:
        raise WindowsRuntimeContractError("runtime package set is not exact and sorted")
    referenced_sources = {package.source_archive for package in runtime_packages}
    if referenced_sources != set(source_by_name):
        raise WindowsRuntimeContractError("runtime packages do not close over every source archive")

    return WindowsRuntimeContract(
        install_packages=tuple(install_packages),
        runtime=root["runtime"],
        runtime_packages=tuple(runtime_packages),
        schema_version=root["schema_version"],
        source_archives=tuple(sources),
    )


def self_test() -> None:
    contract = load_contract()
    if len(contract.install_packages) != 5 or len(contract.source_archives) != 3:
        raise WindowsRuntimeContractError("checked-in runtime contract has unexpected cardinality")
    with tempfile.TemporaryDirectory(prefix="gnfs-windows-runtime-contract-") as directory:
        raw = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
        mutations = []
        unknown_source = json.loads(json.dumps(raw))
        unknown_source["runtime_packages"][0]["source_archive"] = "unknown.src.tar.zst"
        mutations.append(("unknown source archive", unknown_source))
        extra_dll = json.loads(json.dumps(raw))
        extra_dll["runtime_packages"][0]["dll_sha256"]["libntl-45.dll"] = "0" * 64
        extra_dll["runtime_packages"][0]["dll_sha256"] = dict(
            sorted(extra_dll["runtime_packages"][0]["dll_sha256"].items())
        )
        mutations.append(("expanded DLL closure", extra_dll))
        extra_license = json.loads(json.dumps(raw))
        extra_license["runtime_packages"][1]["fallback_licenses"].append(
            {
                "archive_name": "COPYING.LESSERv3",
                "path": (
                    "apps/macos/GNFSWorkbench/GNFSWorkbench/Resources/Licenses/"
                    "GMP-COPYING.LESSERv3.txt"
                ),
                "sha256": "0" * 64,
            }
        )
        mutations.append(("expanded fallback license selection", extra_license))
        for index, (label, mutation) in enumerate(mutations):
            invalid_path = Path(directory) / f"invalid-{index}.json"
            invalid_path.write_text(json.dumps(mutation), encoding="utf-8")
            try:
                load_contract(invalid_path)
            except WindowsRuntimeContractError:
                pass
            else:
                raise WindowsRuntimeContractError(f"contract accepted {label}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("self-test",))
    arguments = parser.parse_args()
    try:
        if arguments.command == "self-test":
            self_test()
            print("Windows runtime source contract self-test: PASS")
    except WindowsRuntimeContractError as error:
        print(f"Windows runtime source contract error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
