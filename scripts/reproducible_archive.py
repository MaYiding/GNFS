#!/usr/bin/env python3
"""Create normalized GNFS release archives without overwriting outputs."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
import time
import zipfile


ZIP_EPOCH = 315532800  # 1980-01-01T00:00:00Z, the earliest ZIP timestamp.


class ArchiveContractError(RuntimeError):
    """Raised when an archive input violates the release contract."""


def _normalized_mode(path: Path) -> int:
    return 0o755 if path.stat().st_mode & 0o111 else 0o644


def _members(source: Path) -> list[Path]:
    members = sorted(source.rglob("*"), key=lambda path: path.relative_to(source).as_posix())
    for member in members:
        if member.is_symlink():
            raise ArchiveContractError(f"release trees may not contain symlinks: {member}")
        if not member.is_dir() and not member.is_file():
            raise ArchiveContractError(f"unsupported release tree entry: {member}")
    return members


def _archive_name(root_name: str, source: Path, member: Path | None = None) -> str:
    if member is None:
        return root_name
    relative = member.relative_to(source)
    return str(PurePosixPath(root_name, *relative.parts))


def _tar_info(name: str, mode: int, epoch: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = epoch
    return info


def _create_tar_gz(source: Path, root_name: str, output: Path, epoch: int) -> None:
    with tempfile.TemporaryDirectory(prefix="gnfs-release-tar-", dir=output.parent) as temp_dir:
        raw_tar = Path(temp_dir) / "archive.tar"
        compressed = Path(temp_dir) / "archive.tar.gz"
        with tarfile.open(raw_tar, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            root_info = _tar_info(root_name, 0o755, epoch)
            root_info.type = tarfile.DIRTYPE
            archive.addfile(root_info)
            for member in _members(source):
                name = _archive_name(root_name, source, member)
                if member.is_dir():
                    info = _tar_info(name, 0o755, epoch)
                    info.type = tarfile.DIRTYPE
                    archive.addfile(info)
                    continue
                info = _tar_info(name, _normalized_mode(member), epoch)
                info.size = member.stat().st_size
                with member.open("rb") as handle:
                    archive.addfile(info, handle)

        with raw_tar.open("rb") as source_handle, compressed.open("xb") as output_handle:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=output_handle, compresslevel=9, mtime=epoch
            ) as gzip_handle:
                shutil.copyfileobj(source_handle, gzip_handle)
        _publish_without_overwrite(compressed, output)


def _create_zip(source: Path, root_name: str, output: Path, epoch: int) -> None:
    normalized_epoch = max(epoch, ZIP_EPOCH)
    date_time = time.gmtime(normalized_epoch)[:6]
    with tempfile.TemporaryDirectory(prefix="gnfs-release-zip-", dir=output.parent) as temp_dir:
        staged = Path(temp_dir) / "archive.zip"
        with zipfile.ZipFile(staged, mode="x", compression=zipfile.ZIP_STORED) as archive:
            root_info = zipfile.ZipInfo(f"{root_name}/", date_time=date_time)
            root_info.create_system = 3
            root_info.external_attr = (stat.S_IFDIR | 0o755) << 16
            archive.writestr(root_info, b"")
            for member in _members(source):
                name = _archive_name(root_name, source, member)
                if member.is_dir():
                    info = zipfile.ZipInfo(f"{name}/", date_time=date_time)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFDIR | 0o755) << 16
                    archive.writestr(info, b"")
                    continue
                info = zipfile.ZipInfo(name, date_time=date_time)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | _normalized_mode(member)) << 16
                info.compress_type = zipfile.ZIP_STORED
                archive.writestr(info, member.read_bytes())
        _publish_without_overwrite(staged, output)


def _publish_without_overwrite(staged: Path, output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise ArchiveContractError(f"refusing to overwrite release archive: {output}")
    try:
        os.link(staged, output)
    except FileExistsError as error:
        raise ArchiveContractError(f"refusing to overwrite release archive: {output}") from error
    except OSError:
        # GitHub's Windows filesystem can reject hard links. Opening with xb
        # preserves the no-overwrite contract on that platform.
        try:
            with staged.open("rb") as source_handle, output.open("xb") as output_handle:
                shutil.copyfileobj(source_handle, output_handle)
        except FileExistsError as error:
            raise ArchiveContractError(
                f"refusing to overwrite release archive: {output}"
            ) from error


def create_archive(source: Path, output: Path, archive_format: str, epoch: int) -> None:
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir() or source.is_symlink():
        raise ArchiveContractError(f"release source must be a real directory: {source}")
    if not source.name or source.name in {".", ".."}:
        raise ArchiveContractError("release source must have a stable root name")
    if epoch < 0:
        raise ArchiveContractError("SOURCE_DATE_EPOCH must be nonnegative")
    if output == source or source in output.parents:
        raise ArchiveContractError("release output must be outside the archived source tree")
    output.parent.mkdir(parents=True, exist_ok=True)
    if archive_format == "tar.gz":
        if not output.name.endswith(".tar.gz"):
            raise ArchiveContractError("tar.gz output must end in .tar.gz")
        _create_tar_gz(source, source.name, output, epoch)
    elif archive_format == "zip":
        if output.suffix != ".zip":
            raise ArchiveContractError("ZIP output must end in .zip")
        _create_zip(source, source.name, output, epoch)
    else:
        raise ArchiveContractError(f"unsupported archive format: {archive_format}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def self_test() -> None:
    epoch = 1_700_000_000
    with tempfile.TemporaryDirectory(prefix="gnfs-archive-self-test-") as temp_dir:
        root = Path(temp_dir)
        source = root / "gnfs-v0.1.0-test"
        (source / "bin").mkdir(parents=True)
        executable = source / "bin" / "gnfs"
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        (source / "README-release.txt").write_text("GNFS v0.1.0\n", encoding="utf-8")

        first_tar = root / "first.tar.gz"
        second_tar = root / "second.tar.gz"
        first_zip = root / "first.zip"
        second_zip = root / "second.zip"
        create_archive(source, first_tar, "tar.gz", epoch)
        create_archive(source, first_zip, "zip", epoch)

        os.utime(executable, (epoch + 500, epoch + 500))
        os.utime(source / "README-release.txt", (epoch + 900, epoch + 900))
        create_archive(source, second_tar, "tar.gz", epoch)
        create_archive(source, second_zip, "zip", epoch)
        if _sha256(first_tar) != _sha256(second_tar):
            raise ArchiveContractError("normalized tar.gz output is not reproducible")
        if _sha256(first_zip) != _sha256(second_zip):
            raise ArchiveContractError("normalized ZIP output is not reproducible")

        with tarfile.open(first_tar, mode="r:gz") as archive:
            members = archive.getmembers()
            if not members or any(member.mtime != epoch for member in members):
                raise ArchiveContractError("tar.gz timestamps are not normalized")
            binary = archive.getmember("gnfs-v0.1.0-test/bin/gnfs")
            if binary.mode != 0o755 or binary.uid != 0 or binary.gid != 0:
                raise ArchiveContractError("tar.gz ownership or modes are not normalized")

        with zipfile.ZipFile(first_zip) as archive:
            binary = archive.getinfo("gnfs-v0.1.0-test/bin/gnfs")
            if binary.compress_type != zipfile.ZIP_STORED:
                raise ArchiveContractError("ZIP entries must use stable stored encoding")
            if (binary.external_attr >> 16) & 0o777 != 0o755:
                raise ArchiveContractError("ZIP executable mode is not normalized")

        existing = root / "existing.zip"
        existing.write_bytes(b"keep")
        try:
            create_archive(source, existing, "zip", epoch)
        except ArchiveContractError:
            pass
        else:
            raise ArchiveContractError("archive creation overwrote an existing output")
        if existing.read_bytes() != b"keep":
            raise ArchiveContractError("existing output changed after overwrite rejection")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create", help="create one normalized archive")
    create.add_argument("--source", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--format", choices=("tar.gz", "zip"), required=True)
    create.add_argument("--epoch", type=int, required=True)
    subparsers.add_parser("self-test", help="run archive contract tests")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "self-test":
            self_test()
            print("reproducible archive self-test: PASS")
        else:
            create_archive(arguments.source, arguments.output, arguments.format, arguments.epoch)
    except (ArchiveContractError, OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"reproducible archive error: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
