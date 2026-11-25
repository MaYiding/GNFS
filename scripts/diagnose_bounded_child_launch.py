#!/usr/bin/env python3
"""Linux-only, bounded, on-demand pre-exec stack capture; never an acceptance gate.

Run the existing ASan test without ptrace until a direct, still-pre-exec child
waits in a futex for 250 ms. Attach once, detach, and retain the test's own exit.
The 30-second limit belongs to this observer, not to CTest (whose sanitizer
timeout is scaled). No product deadline or executable is changed.
"""

import argparse
import dataclasses
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone


ATTEMPTS = 6
RUN_SECONDS = 30.0
POLL_SECONDS = 0.025
STALL_SECONDS = 0.25
CAPTURE_MARKER = "GNFS_PREEXEC_STACK_CAPTURED"


@dataclasses.dataclass(frozen=True)
class Identity:
    pid: int
    starttime: int
    ppid: int
    pgid: int
    exe_dev: int
    exe_ino: int


def stamp():
    return {"utc": datetime.now(timezone.utc).isoformat(),
            "monotonic": time.monotonic()}


def parse_stat(text):
    # comm can contain spaces and parentheses; fields after its final ')' are
    # state (3), ppid (4), pgrp (5), ... starttime (22).
    pid_text, separator, tail = text.partition(" (")
    _, closing, fields_text = tail.rpartition(") ")
    if not separator or not closing:
        raise ValueError("malformed proc stat")
    fields = fields_text.split()
    return (int(pid_text), fields[0], int(fields[1]), int(fields[2]),
            int(fields[19]))


def read_identity(pid, proc_root=Path("/proc")):
    directory = proc_root / str(pid)
    before = parse_stat((directory / "stat").read_text())
    executable = (directory / "exe").stat()
    after = parse_stat((directory / "stat").read_text())
    if before[0] != pid or before[0:1] + before[2:] != after[0:1] + after[2:]:
        raise ValueError("process identity changed during observation")
    return Identity(pid, before[4], before[2], before[3],
                    executable.st_dev, executable.st_ino)


def status_fields(text):
    return dict(line.split(":", 1) for line in text.splitlines() if ":" in line)


def is_same_process(identity, proc_root=Path("/proc")):
    try:
        return read_identity(identity.pid, proc_root) == identity
    except (OSError, ValueError, IndexError):
        return False


def direct_children(parent_pid, proc_root=Path("/proc")):
    """Best-effort live snapshot, not a proof that absent children do not exist."""
    result = set()
    for entry in (proc_root / str(parent_pid) / "task").iterdir():
        try:
            result.update(int(pid) for pid in (entry / "children").read_text().split())
        except (OSError, ValueError):
            continue
    return result


def candidate(pid, parent, proc_root=Path("/proc")):
    identity = read_identity(pid, proc_root)
    if (identity.ppid != parent.pid or identity.pgid != pid or
            (identity.exe_dev, identity.exe_ino) != (parent.exe_dev, parent.exe_ino)):
        return None
    status = status_fields((proc_root / str(pid) / "status").read_text())
    wchan = (proc_root / str(pid) / "wchan").read_text().strip()
    # A LeakSanitizer stop-the-world helper can share the executable, but does
    # not create the separate child process group used by this transport.
    if (int(status.get("Threads", "0")) != 1 or
            int(status.get("TracerPid", "-1")) != 0 or "futex" not in wchan):
        return None
    if not is_same_process(identity, proc_root):
        return None
    return identity, wchan


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def snapshot(identity, directory):
    directory.mkdir()
    metadata = {"identity": dataclasses.asdict(identity), "timestamp": stamp()}
    if not is_same_process(identity):
        metadata["error"] = "identity changed before snapshot"
        write_json(directory / "metadata.json", metadata)
        return
    proc = Path("/proc") / str(identity.pid)
    for name in ("status", "wchan", "syscall", "maps", "stat"):
        try:
            (directory / name).write_text((proc / name).read_text())
        except OSError as error:
            (directory / (name + ".error")).write_text(str(error) + "\n")
    descriptors = {}
    try:
        for descriptor in sorted((proc / "fd").iterdir()):
            try:
                descriptors[descriptor.name] = os.readlink(descriptor)
            except OSError as error:
                descriptors[descriptor.name] = "unavailable: " + str(error)
    except OSError as error:
        metadata["fd_error"] = str(error)
    write_json(directory / "fd.json", descriptors)
    metadata["identity_still_matches"] = is_same_process(identity)
    write_json(directory / "metadata.json", metadata)


def gdb_command(identity):
    expected = dataclasses.asdict(identity)
    # Check again after attach has stopped the inferior: an exec or PID reuse
    # between the monitor snapshot and attach must never be reported as proof.
    code = """
import gdb, os
expected = EXPECTED
pid = expected['pid']
try:
    gdb.execute('attach %d' % pid)
    root = '/proc/%d/' % pid
    stat = open(root + 'stat').read().rsplit(') ', 1)[1].split()
    exe = os.stat(root + 'exe')
    status = dict(line.split(':', 1) for line in open(root + 'status') if ':' in line)
    actual = dict(pid=pid, starttime=int(stat[19]), ppid=int(stat[1]),
                  pgid=int(stat[2]), exe_dev=exe.st_dev, exe_ino=exe.st_ino)
    if actual != expected or int(status['Threads']) != 1 or int(status['TracerPid']) != os.getpid():
        raise gdb.GdbError('pre-exec identity changed after attach; no stack accepted')
    gdb.execute('thread apply all bt 32')
    print('GNFS_PREEXEC_STACK_CAPTURED')
finally:
    if gdb.selected_inferior().pid:
        gdb.execute('detach')
""".replace("EXPECTED", repr(expected))
    # timeout runs as root too: an unprivileged observer cannot reliably kill
    # a root gdb through sudo. Its own process group is bounded by TERM + KILL.
    return ["sudo", "-n", "timeout", "--signal=TERM", "--kill-after=0.25s", "1.75s",
            "gdb", "-nx", "-batch", "-quiet",
            "-iex", "set auto-load off", "-iex", "set debuginfod enabled off",
            "-iex", "set pagination off", "-iex", "set confirm off",
            "-ex", "python exec(" + repr(code) + ")"]


def capture(identity, directory):
    result = {"started": stamp(), "identity": dataclasses.asdict(identity),
              "observer_attached": "possible", "diagnostic_only": True}
    snapshot(identity, directory / "before-attach")
    if not is_same_process(identity):
        result["observer_error"] = "identity changed before debugger launch"
        result["stack_captured"] = False
        return result
    with (directory / "gdb.log").open("w") as log:
        debugger = subprocess.Popen(gdb_command(identity), stdout=log,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                    start_new_session=True)
        try:
            # Includes sudo setup. The root-side watchdog bounds gdb to 2 s.
            result["debugger_exit"] = debugger.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            result["launcher_timeout"] = True
            # Normally timeout has already killed its root-owned group. This
            # fallback kills only our own sudo launcher session, never a test.
            try:
                os.killpg(debugger.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError) as error:
                result["launcher_cleanup_error"] = str(error)
            try:
                result["debugger_exit"] = debugger.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                result["launcher_not_reaped"] = True
    result["stack_captured"] = CAPTURE_MARKER in (directory / "gdb.log").read_text().splitlines()
    if result.get("launcher_timeout") or result.get("launcher_not_reaped"):
        result["observer_error"] = "debugger launcher exceeded its outer bound"
    elif result.get("debugger_exit") != 0:
        result["observer_error"] = "debugger did not complete successfully"
    elif not result["stack_captured"]:
        result["observer_error"] = "debugger returned without a verified stack capture"
    result["finished"] = stamp()
    if is_same_process(identity):
        status = status_fields((Path("/proc") / str(identity.pid) / "status").read_text())
        result["post_attach_tracer_pid"] = int(status.get("TracerPid", "-1"))
        result["post_attach_state"] = status.get("State", "").strip()
        if result["post_attach_tracer_pid"] != 0:
            result["observer_error"] = "child still has a tracer after debugger returned"
        elif result["post_attach_state"].startswith(("T", "t")):
            result["observer_error"] = "child remains stopped after debugger returned"
        # Normal gdb detach resumes execution. Do not send an indiscriminate
        # SIGCONT if abnormal debugger teardown left a stop; retain evidence.
        snapshot(identity, directory / "after-attach")
    else:
        result["post_attach_identity"] = "exited, execed, or changed"
    return result


def signal_verified(identity, sig):
    """A pidfd pins signal delivery; identity checks reject an exec as well."""
    try:
        descriptor = os.pidfd_open(identity.pid)
    except ProcessLookupError:
        return False
    try:
        if not is_same_process(identity):
            return False
        signal.pidfd_send_signal(descriptor, sig)
        return True
    finally:
        os.close(descriptor)


def run_attempt(argv, build_dir, directory):
    directory.mkdir()
    result = {"started": stamp(), "argv": argv, "external_limit_seconds": RUN_SECONDS,
              "observer_attached": False, "outer_timeout": False,
              "cleanup": [], "execed_children_not_signalled": []}
    with (directory / "stdout.log").open("w") as stdout, (directory / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(argv, cwd=build_dir, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        try:
            parent = read_identity(process.pid)
        except (OSError, ValueError, IndexError) as error:
            result["identity_error"] = str(error)
            result["observer_error"] = "could not establish the started test identity"
            try:
                result["native_exit"] = process.wait(timeout=RUN_SECONDS)
            except subprocess.TimeoutExpired:
                # No identity means no safe external signalling. This is an
                # observer failure, never evidence that the native test passed.
                result["outer_timeout"] = True
                result["cleanup_error"] = "test identity unavailable; no signal sent"
            write_json(directory / "result.json", result)
            return result
        result["test_identity"] = dataclasses.asdict(parent)
        first_seen = {}
        observed = {}
        deadline = result["started"]["monotonic"] + RUN_SECONDS
        try:
            while process.poll() is None and time.monotonic() < deadline:
                active = {}
                try:
                    children = direct_children(parent.pid)
                except OSError:
                    children = set()
                for pid in children:
                    try:
                        child = read_identity(pid)
                        if child.ppid == parent.pid:
                            observed[(child.pid, child.starttime)] = child
                        eligible = candidate(pid, parent)
                    except (OSError, ValueError, IndexError):
                        continue
                    if eligible is None or result["observer_attached"]:
                        continue
                    child, wchan = eligible
                    now = time.monotonic()
                    began = first_seen.get(child, now)
                    active[child] = began
                    if now - began >= STALL_SECONDS and deadline - now >= 3.5:
                        result["observer_attached"] = True
                        result["pre_attach_observation"] = {
                            "first_seen_monotonic": began, "last_seen_monotonic": now,
                            "duration_seconds": now - began, "wchan": wchan}
                        try:
                            result["capture"] = capture(child, directory)
                        except (OSError, ValueError, IndexError) as error:
                            result["capture"] = {"observer_error": str(error),
                                                 "stack_captured": False}
                        break
                first_seen = active
                time.sleep(POLL_SECONDS)
            if process.poll() is None:
                result["outer_timeout"] = True
        finally:
            if process.poll() is None:
                # Signal only identity-verified direct children still carrying
                # the test executable. Execed helpers/descendants are outside
                # this observer's cleanup authority; record that boundary.
                for child in observed.values():
                    if (child.pgid != child.pid or
                            (child.exe_dev, child.exe_ino) != (parent.exe_dev, parent.exe_ino)):
                        if is_same_process(child):
                            result["execed_children_not_signalled"].append(dataclasses.asdict(child))
                        continue
                    try:
                        if signal_verified(child, signal.SIGKILL):
                            result["cleanup"].append({"pid": child.pid, "signal": "SIGKILL", "kind": "pre-exec child"})
                    except (OSError, ValueError, IndexError) as error:
                        result["cleanup"].append({"pid": child.pid, "error": str(error)})
                try:
                    if signal_verified(parent, signal.SIGKILL):
                        result["cleanup"].append({"pid": parent.pid, "signal": "SIGKILL", "kind": "test"})
                except (OSError, ValueError, IndexError) as error:
                    result["cleanup"].append({"pid": parent.pid, "error": str(error)})
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    result["cleanup_error"] = "test did not reap within 1 second"
        result["native_exit"] = process.returncode
    result["finished"] = stamp()
    result["elapsed_seconds"] = result["finished"]["monotonic"] - result["started"]["monotonic"]
    result["exit_may_be_observer_affected"] = result["observer_attached"] or bool(result["cleanup"])
    observer_errors = []
    for source in (result, result.get("capture", {})):
        for key in ("observer_error", "cleanup_error", "launcher_cleanup_error"):
            if source.get(key):
                observer_errors.append(source[key])
    observer_errors.extend(item["error"] for item in result["cleanup"] if "error" in item)
    result["observer_errors"] = observer_errors
    result["result_is_acceptance"] = False
    write_json(directory / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this diagnostic requires Linux /proc and pidfds")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        parser.error("Python must support pidfd_open and pidfd_send_signal")
    for command in ("sudo", "timeout", "gdb"):
        if shutil.which(command) is None:
            parser.error(command + " is required")
    # The workflow supplies leak detection. Do not silently disable or rewrite
    # sanitizer settings in a diagnostic that claims to retain the baseline.
    asan = dict(item.split("=", 1) for item in os.environ.get("ASAN_OPTIONS", "").split(":") if "=" in item)
    if asan.get("detect_leaks") != "1":
        parser.error("ASAN_OPTIONS must explicitly retain detect_leaks=1")
    build_dir = args.build_dir.resolve(strict=True)
    argv = [str((build_dir / name).resolve(strict=True)) for name in (
        "test_bounded_child_process", "bounded_child_process_fake_child",
        "bounded_child_process_authenticated_supervisor")]
    if not all(os.access(path, os.X_OK) for path in argv):
        parser.error("the existing test and both helpers must be executable")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    summary = {"diagnostic_only": True, "product_deadline_unchanged": True,
               "ctest_timeout_unchanged": True, "max_attempts": ATTEMPTS,
               "cleanup_boundary": "identity-verified test and still-pre-exec direct children only; no execed helpers or descendants are signalled",
               "attempts": []}
    for number in range(1, ATTEMPTS + 1):
        result = run_attempt(argv, build_dir, output / ("attempt-%02d" % number))
        summary["attempts"].append(result)
        write_json(output / "summary.json", summary)
        print(json.dumps({"attempt": number, "native_exit": result.get("native_exit"),
                          "outer_timeout": result["outer_timeout"],
                          "capture_attempted": result["observer_attached"],
                          "observer_errors": result.get("observer_errors", [])}), flush=True)
        if result["observer_attached"] or result["outer_timeout"] or result.get("cleanup_error"):
            break
    summary["stack_captured"] = any(item.get("capture", {}).get("stack_captured", False) for item in summary["attempts"])
    summary["test_failed_or_not_completed"] = any(item.get("native_exit") != 0 or item["outer_timeout"] for item in summary["attempts"])
    summary["observer_error_observed"] = any(item.get("observer_errors") or item.get("observer_error") for item in summary["attempts"])
    write_json(output / "summary.json", summary)
    return int(summary["test_failed_or_not_completed"] or summary["observer_error_observed"])


if __name__ == "__main__":
    sys.exit(main())
