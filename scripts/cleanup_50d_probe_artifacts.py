#!/usr/bin/env python3
"""Remove only the permanent lock files from a successful 50-digit probe."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import tempfile
from pathlib import Path


RAW_LOCK = "raw.gnfs-ooc-cleanup-v1.lock"
STRUCTURED_LOCK = re.compile(
    r"raw\.gnfs-structured-run-p[1-9][0-9]*-r1\.g1\.output"
    r"\.gnfs-sink-lease\.gnfs-ooc-cleanup-v1\.lock\Z"
)


class ProbeCleanupError(RuntimeError):
    """The successful probe directory did not match the exact cleanup contract."""


def _directory_flags() -> int:
    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_DIRECTORY", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    return flags


def _same_identity(left: os.stat_result, right: os.stat_result) -> bool:
    return left.st_dev == right.st_dev and left.st_ino == right.st_ino


def _expected_names(names: list[str], route: str) -> list[str]:
    if route == "legacy":
        if names != [RAW_LOCK]:
            raise ProbeCleanupError(
                f"legacy probe residue differs from the permanent raw lock: {names}"
            )
        return names
    if route != "structured":
        raise ProbeCleanupError(f"unsupported probe route: {route}")
    structured = [name for name in names if STRUCTURED_LOCK.fullmatch(name)]
    if len(names) != 2 or RAW_LOCK not in names or len(structured) != 1:
        raise ProbeCleanupError(
            "structured probe residue must be the raw lock plus one exact "
            f"structured-output lock: {names}"
        )
    return names


def cleanup_successful_probe_directory(directory: Path, route: str) -> None:
    if not directory.is_absolute():
        raise ProbeCleanupError("probe directory must be an absolute path")
    if directory.name in {"", ".", ".."} or directory.parent == directory:
        raise ProbeCleanupError("probe directory target is unsafe")

    parent_fd = os.open(directory.parent, _directory_flags())
    directory_fd = -1
    try:
        entry_status = os.stat(
            directory.name, dir_fd=parent_fd, follow_symlinks=False
        )
        if not stat.S_ISDIR(entry_status.st_mode):
            raise ProbeCleanupError("probe directory target is not a directory")
        directory_fd = os.open(
            directory.name, _directory_flags(), dir_fd=parent_fd
        )
        opened_status = os.fstat(directory_fd)
        if not _same_identity(entry_status, opened_status):
            raise ProbeCleanupError("probe directory identity changed while opening")

        names = sorted(os.listdir(directory_fd))
        expected = _expected_names(names, route)
        for name in expected:
            residue = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if (
                not stat.S_ISREG(residue.st_mode)
                or residue.st_nlink != 1
                or residue.st_size != 0
            ):
                raise ProbeCleanupError(
                    f"probe residue is not an owned zero-byte single-link file: {name}"
                )

        current_entry = os.stat(
            directory.name, dir_fd=parent_fd, follow_symlinks=False
        )
        if not _same_identity(opened_status, current_entry):
            raise ProbeCleanupError("probe directory identity changed before cleanup")
        for name in expected:
            os.unlink(name, dir_fd=directory_fd)
        if os.listdir(directory_fd):
            raise ProbeCleanupError("probe directory gained residue during cleanup")
        current_entry = os.stat(
            directory.name, dir_fd=parent_fd, follow_symlinks=False
        )
        if not _same_identity(opened_status, current_entry):
            raise ProbeCleanupError("probe directory identity changed before removal")
        os.rmdir(directory.name, dir_fd=parent_fd)
    except OSError as error:
        raise ProbeCleanupError(f"probe lock cleanup failed: {error}") from error
    finally:
        if directory_fd >= 0:
            os.close(directory_fd)
        os.close(parent_fd)


def _write(path: Path, payload: bytes = b"") -> None:
    path.write_bytes(payload)


def _expect_rejected(directory: Path, route: str) -> None:
    before = sorted(path.name for path in directory.iterdir())
    try:
        cleanup_successful_probe_directory(directory, route)
    except ProbeCleanupError:
        pass
    else:
        raise AssertionError(f"unsafe probe residue was accepted: {directory}")
    if not directory.is_dir() or sorted(path.name for path in directory.iterdir()) != before:
        raise AssertionError("rejected probe residue was mutated")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="gnfs-50d-lock-cleanup-selftest-") as root_text:
        root = Path(root_text)

        legacy = root / "legacy-valid"
        legacy.mkdir()
        _write(legacy / RAW_LOCK)
        cleanup_successful_probe_directory(legacy, "legacy")
        if legacy.exists():
            raise AssertionError("valid legacy probe directory survived cleanup")

        structured = root / "structured-valid"
        structured.mkdir()
        _write(structured / RAW_LOCK)
        _write(
            structured
            / "raw.gnfs-structured-run-p123-r1.g1.output.gnfs-sink-lease."
            "gnfs-ooc-cleanup-v1.lock"
        )
        cleanup_successful_probe_directory(structured, "structured")
        if structured.exists():
            raise AssertionError("valid structured probe directory survived cleanup")

        fixtures: list[tuple[str, str]] = [
            ("extra", "legacy"),
            ("nonzero", "legacy"),
            ("symlink", "legacy"),
            ("hardlink", "legacy"),
            ("missing-structured", "structured"),
            ("wrong-structured", "structured"),
        ]
        for label, route in fixtures:
            directory = root / label
            directory.mkdir()
            if label == "hardlink":
                anchor = root / "hardlink-anchor"
                _write(anchor)
                os.link(anchor, directory / RAW_LOCK)
            elif label == "symlink":
                anchor = root / "symlink-anchor"
                _write(anchor)
                os.symlink(anchor, directory / RAW_LOCK)
            else:
                _write(directory / RAW_LOCK, b"x" if label == "nonzero" else b"")
            if label == "extra":
                _write(directory / "unexpected")
            if label == "wrong-structured":
                _write(directory / "raw.gnfs-structured-run-p0-r1.g1.output.gnfs-sink-lease."
                       "gnfs-ooc-cleanup-v1.lock")
            _expect_rejected(directory, route)
            shutil.rmtree(directory)

        target = root / "directory-target"
        target.mkdir()
        directory_link = root / "directory-link"
        directory_link.symlink_to(target, target_is_directory=True)
        try:
            cleanup_successful_probe_directory(directory_link, "legacy")
        except ProbeCleanupError:
            pass
        else:
            raise AssertionError("probe directory symlink was accepted")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    cleanup = subparsers.add_parser("cleanup")
    cleanup.add_argument("--directory", type=Path, required=True)
    cleanup.add_argument("--route", choices=("legacy", "structured"), required=True)
    subparsers.add_parser("self-test")
    arguments = parser.parse_args()
    try:
        if arguments.command == "cleanup":
            cleanup_successful_probe_directory(arguments.directory, arguments.route)
        else:
            self_test()
            print("50-digit probe lock cleanup self-test: PASS")
    except ProbeCleanupError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
