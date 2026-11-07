"""Exercise snapshot publication collisions and failures through the manual V3 runner."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
PROFILE = "test_siqs_256a_scale_profile"
SUPERVISOR = "gnfs_test_process_supervisor"


def exercise(binaries, root):
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\nproject(SnapshotPublication NONE)\n"
        f"add_custom_target({PROFILE})\nadd_custom_target({SUPERVISOR})\n"
    )
    build = root / "build"
    subprocess.run(["cmake", "-S", root, "-B", build, "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, capture_output=True, timeout=30)
    shutil.copy2(binaries / SUPERVISOR, build / SUPERVISOR)
    # The profile is already running by the time it creates a competing entry.
    # Pass valid captures and replays through to the real Release binary.
    wrapper = build / PROFILE
    wrapper.write_text(
        '#!/bin/sh\nset -eu\ncase "$TEST_ACTION" in\n'
        '  race) mkdir "$TEST_DESTINATION";;\n'
        '  fail) exit 23;;\n'
        'esac\n"$TEST_PROFILE" "$@"\n'
        'if [ "$TEST_ACTION" = malformed ]; then echo unexpected-seventh-record; fi\n'
    )
    wrapper.chmod(0o755)
    target = build / "siqs-256a-example.bin"
    capture = ["2", "--snapshot", "example"]
    replay = ["--replay", "example"]

    def run(args, code, message, action="pass"):
        result = subprocess.run(
            [PROJECT / "scripts/test.sh", "-j", "2", "--no-color", "profile-siqs-256a", *args],
            cwd=PROJECT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=150,
            env={**os.environ, "GNFS_BUILD_DIR": str(build), "TMPDIR": str(root),
                 "TEST_PROFILE": str(binaries / PROFILE), "TEST_ACTION": action,
                 "TEST_DESTINATION": str(target)},
        )
        if result.returncode != code or message not in result.stdout:
            raise RuntimeError(f"{args} ({action}): exit={result.returncode}\n{result.stdout}")
        if code and any(line.startswith("GNFS_SIQS_256A_") for line in result.stdout.splitlines()):
            raise RuntimeError("unsuccessful runner forwarded profile evidence")
        if list(build.glob(".siqs-256a-rows.*")):
            raise RuntimeError("capture left a staging directory")

    run(capture, 1, "Cannot publish V3 row snapshot", action="race")
    if not target.is_dir() or any(target.iterdir()):
        raise RuntimeError("capture replaced the competing empty directory")
    run(capture, 1, "Snapshot already exists")
    run(replay, 2, "missing V3 row snapshot")
    target.rmdir()
    print("PASS concurrent empty directory and existing directory")

    sentinel = root / "sentinel"
    sentinel.write_bytes(b"preserve")
    for destination in (sentinel, root / "missing"):
        target.symlink_to(destination)
        run(capture, 1, "Snapshot already exists")
        run(replay, 2, "missing V3 row snapshot")
        if target.readlink() != destination or sentinel.read_bytes() != b"preserve":
            raise RuntimeError("capture changed the existing link or its target")
        target.unlink()
    print("PASS existing and dangling symlinks")

    run(["2"], 1, "退出码 23", action="fail")
    run(["2"], 1, "六行 V3 transcript", action="malformed")
    run(capture, 1, "六行 V3 transcript", action="malformed")
    if target.exists() or target.is_symlink():
        raise RuntimeError("capture published rows from an invalid transcript")
    print("PASS unsuccessful stock process and malformed stock or staged output")

    run(capture, 0, "V3 snapshot:")
    before = target.read_bytes()
    run(capture, 1, "Snapshot already exists")
    if target.read_bytes() != before:
        raise RuntimeError("repeated capture changed an existing snapshot")
    run(replay, 0, "backend=sparse_only")
    print("PASS regular-file publication, repeat capture, and fresh replay")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="compiled Release profile and supervisor directory")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="siqs-256a-publication-") as temporary:
        exercise(args.build.resolve(strict=True), Path(temporary).resolve())


if __name__ == "__main__":
    main()
