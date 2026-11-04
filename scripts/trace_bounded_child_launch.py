#!/usr/bin/env python3
"""Linux strace diagnostic: briefly stop one already-stalled pre-exec child.

This perturbs scheduling and is never acceptance evidence. The original test,
its five-second launch deadline, and its executable are unchanged. Each of twelve
attempts has a 30-second observer limit. No futex return values are injected.
"""

import argparse
import dataclasses
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import time

import diagnose_bounded_child_launch as diag


ATTEMPTS = 12
RUN_SECONDS = 30.0
GROUP_STOP_SECONDS = 0.75
RESUME_SECONDS = 0.25
interrupted_signal = 0


def request_shutdown(signum, _frame):
    # Do not raise asynchronously inside the STOP/CONT cleanup obligation.
    global interrupted_signal
    interrupted_signal = signum


def proc_status(pid):
    return diag.status_fields((Path("/proc") / str(pid) / "status").read_text())


def traced_candidate(pid, parent, tracer_pid):
    identity = diag.read_identity(pid)
    if (identity.ppid != parent.pid or identity.pgid != pid or
            (identity.exe_dev, identity.exe_ino) != (parent.exe_dev, parent.exe_ino)):
        return None
    proc = Path("/proc") / str(pid)
    status = proc_status(pid)
    wchan = (proc / "wchan").read_text().strip()
    syscall = (proc / "syscall").read_text().strip()
    if (int(status.get("Threads", "0")) != 1 or
            int(status.get("TracerPid", "-1")) != tracer_pid or
            "futex" not in wchan or syscall == "running" or
            not diag.is_same_process(identity)):
        return None
    # Include the syscall address/arguments: consecutive unrelated waits must
    # not accumulate into a supposedly stable 250 ms observation.
    return identity, wchan, syscall


def find_test(tracer_pid, executable_identity):
    matches = []
    for pid in diag.direct_children(tracer_pid):
        try:
            identity = diag.read_identity(pid)
            status = proc_status(pid)
            if (identity.ppid == tracer_pid and
                    (identity.exe_dev, identity.exe_ino) == executable_identity and
                    int(status.get("TracerPid", "-1")) == tracer_pid):
                matches.append(identity)
        except (OSError, ValueError, IndexError):
            continue
    if len(matches) > 1:
        raise RuntimeError("strace has multiple matching direct test children")
    return matches[0] if matches else None


def new_trace_text(path, offset):
    try:
        with path.open("rb") as stream:
            stream.seek(offset)
            return stream.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def group_stop_seen(text, sender_pid):
    # The delivery line is printed BEFORE strace forwards SIGSTOP. Sending
    # SIGCONT then can cancel the pending stop, followed by a late real stop.
    # Only the subsequent group-stop event proves SIGSTOP has taken effect.
    delivery = re.search(r"--- SIGSTOP \{[^\n]*\bsi_pid=" + str(sender_pid) +
                         r"(?:,|\})[^\n]* ---", text)
    return (delivery is not None and
            "--- stopped by SIGSTOP ---" in text[delivery.end():])


def stop_stack_evidence(text, sender_pid):
    delivery = re.search(r"--- SIGSTOP \{[^\n]*\bsi_pid=" + str(sender_pid) +
                         r"(?:,|\})[^\n]* ---", text)
    if delivery is None:
        return []
    group = text.find("--- stopped by SIGSTOP ---", delivery.end())
    if group < 0:
        return []
    frames = []
    for offset in (delivery.end(), group + len("--- stopped by SIGSTOP ---")):
        for line in text[offset:].splitlines():
            if not line.strip():
                continue
            if not line.startswith(" > "):
                break
            # libunwind errors also start with ' > '. Require a real mapped
            # module and address, directly following OUR signal event. Earlier
            # syscall stacks and error-only unwinds are not capture evidence.
            if re.fullmatch(r" > (?:/[^\n]*|\[[^\]\n]+\]) \[0x[0-9a-fA-F]+\]", line):
                frames.append(line)
    return frames


def capture(child, parent, tracer_pid, trace_path, directory, deadline):
    result = {"identity": dataclasses.asdict(child), "started": diag.stamp(),
              "stop_sent": False, "group_stop_seen": False, "cont_sent": False,
              "stack_captured": False, "diagnostic_only": True}
    descriptor = os.pidfd_open(child.pid)
    try:
        diag.snapshot(child, directory / "before-stop")
        eligible = traced_candidate(child.pid, parent, tracer_pid)
        if eligible is None or eligible[0] != child or interrupted_signal:
            raise RuntimeError("pre-exec identity changed before STOP")
        if deadline - time.monotonic() < GROUP_STOP_SECONDS + RESUME_SECONDS:
            raise RuntimeError("insufficient observer time to stop and resume safely")
        offset = trace_path.stat().st_size if trace_path.exists() else 0
        result["trace_offset_before_stop"] = offset
        signal.pidfd_send_signal(descriptor, signal.SIGSTOP)
        result["stop_sent"] = True
        stop_deadline = min(deadline, time.monotonic() + GROUP_STOP_SECONDS)
        text = ""
        while time.monotonic() < stop_deadline and not interrupted_signal:
            text = new_trace_text(trace_path, offset)
            if group_stop_seen(text, os.getpid()):
                result["group_stop_seen"] = True
                if stop_stack_evidence(text, os.getpid()):
                    break
            time.sleep(diag.POLL_SECONDS)
        if not result["group_stop_seen"]:
            raise RuntimeError("no verified group-stop event before observer bound")
        if not diag.is_same_process(child):
            raise RuntimeError("child image or ownership changed before stack validation")
        status = proc_status(child.pid)
        if (int(status.get("TracerPid", "-1")) != tracer_pid or
                int(status.get("Threads", "0")) != 1):
            raise RuntimeError("child tracer or thread count changed during capture")
        frames = stop_stack_evidence(text, os.getpid())
        result["stack_captured"] = bool(frames)
        if not result["stack_captured"]:
            raise RuntimeError("group-stop was observed without a strace stack")
        result["signal_stack_frames"] = frames[:64]
        delivery_start = re.search(r"--- SIGSTOP \{[^\n]*\bsi_pid=" + str(os.getpid()) +
                                   r"(?:,|\})[^\n]* ---", text).start()
        result["signal_trace_excerpt"] = text[delivery_start:][:16384]
        diag.snapshot(child, directory / "at-group-stop")
    except (OSError, ValueError, IndexError, RuntimeError, KeyboardInterrupt) as error:
        result["observer_error"] = str(error) or "interrupted during capture"
        result["stack_captured"] = False
    finally:
        if result["stop_sent"]:
            try:
                # Keep this original pidfd across STOP and CONT. Once STOP was
                # sent, an exec/reparent race must not cancel our duty to resume
                # that same process. Never replace this with a numeric PID.
                signal.pidfd_send_signal(descriptor, signal.SIGCONT)
                result["cont_sent"] = True
            except ProcessLookupError:
                result["resume_state"] = "original process exited"
            except OSError as error:
                result["observer_error"] = "CONT failed: " + str(error)
            if result["cont_sent"] and result["group_stop_seen"]:
                resume_deadline = min(deadline, time.monotonic() + RESUME_SECONDS)
                while time.monotonic() < resume_deadline and not interrupted_signal:
                    try:
                        if select.select([descriptor], [], [], 0)[0]:
                            result["resume_state"] = "original process exited"
                            break
                        # Inspect state only. Identity changes do not remove
                        # the resume duty, and no new signal is sent by PID.
                        state = diag.parse_stat((Path("/proc") / str(child.pid) / "stat").read_text())[1]
                        if state not in ("T", "t"):
                            result["resume_state"] = state
                            break
                    except (OSError, ValueError, IndexError) as error:
                        if select.select([descriptor], [], [], 0)[0]:
                            result["resume_state"] = "original process exited"
                        else:
                            result["observer_error"] = "resume state could not be verified: " + str(error)
                        break
                    time.sleep(diag.POLL_SECONDS)
                if "resume_state" not in result:
                    result["observer_error"] = "CONT sent but leaving stop was not observed"
        os.close(descriptor)
    # An unconfirmed group-stop can still be pending after an early CONT. The
    # caller must kill its own --kill-on-exit tracer on ANY capture error, so a
    # late STOP cannot strand a tracee after this bounded observer exits.
    result["finished"] = diag.stamp()
    return result


def run_attempt(argv, build_dir, directory, strace):
    directory.mkdir()
    trace_prefix = directory / "trace"
    command = [strace, "--kill-on-exit", "-I", "2", "-ff", "-b", "execve",
               "-ttt", "-T", "-yy", "-s", "128", "-k", "-i",
               "-e", "trace=%process,futex,prctl", "-e", "signal=all",
               "-o", str(trace_prefix), *argv]
    result = {"started": diag.stamp(), "argv": command, "diagnostic_only": True,
              "result_is_acceptance": False, "external_limit_seconds": RUN_SECONDS,
              "capture_attempted": False, "outer_timeout": False, "cleanup": [],
              "observer_errors": [], "exit_may_be_observer_affected": True}
    executable = Path(argv[0]).stat()
    expected_test = (executable.st_dev, executable.st_ino)
    parent = None
    observed = {}
    force_cleanup = False
    with (directory / "test.log").open("w") as log:
        process = subprocess.Popen(command, cwd=build_dir, stdout=log,
                                   stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                   start_new_session=True)
        tracer_fd = None
        try:
            tracer_fd = os.pidfd_open(process.pid)
            deadline = result["started"]["monotonic"] + RUN_SECONDS
            first_seen = {}
            while process.poll() is None and time.monotonic() < deadline and not interrupted_signal:
                if "tracer_identity" not in result:
                    tracer = diag.read_identity(process.pid)
                    tracer_exe = Path(strace).stat()
                    if ((tracer.exe_dev, tracer.exe_ino) == (tracer_exe.st_dev, tracer_exe.st_ino)
                            and tracer.pgid == tracer.pid and tracer.ppid == os.getpid()):
                        result["tracer_identity"] = dataclasses.asdict(tracer)
                if parent is None:
                    parent = find_test(process.pid, expected_test)
                    if parent is not None:
                        result["test_identity"] = dataclasses.asdict(parent)
                    time.sleep(diag.POLL_SECONDS)
                    continue
                active = {}
                try:
                    children = diag.direct_children(parent.pid) if diag.is_same_process(parent) else set()
                except OSError:
                    children = set()
                for pid in children:
                    try:
                        eligible = traced_candidate(pid, parent, process.pid)
                    except (OSError, ValueError, IndexError):
                        continue
                    if eligible is None:
                        continue
                    child, wchan, syscall = eligible
                    observed[child] = child
                    if result["capture_attempted"]:
                        continue
                    now = time.monotonic()
                    began = first_seen.get(eligible, now)
                    active[eligible] = began
                    if now - began >= diag.STALL_SECONDS:
                        result["capture_attempted"] = True
                        result["pre_stop_observation"] = {
                            "first_seen_monotonic": began, "last_seen_monotonic": now,
                            "duration_seconds": now - began, "wchan": wchan, "syscall": syscall}
                        result["capture"] = capture(child, parent, process.pid,
                                                    Path(str(trace_prefix) + "." + str(pid)),
                                                    directory, deadline)
                        if result["capture"].get("observer_error"):
                            result["observer_errors"].append(result["capture"]["observer_error"])
                            force_cleanup = True
                        break
                if force_cleanup:
                    break
                first_seen = active
                time.sleep(diag.POLL_SECONDS)
            result["outer_timeout"] = process.poll() is None and time.monotonic() >= deadline
            if parent is None:
                result["observer_errors"].append("unique direct test child was not established")
            if "tracer_identity" not in result:
                result["observer_errors"].append("strace executable identity was not established")
        except (OSError, ValueError, IndexError, RuntimeError, KeyboardInterrupt) as error:
            result["observer_errors"].append(str(error) or "observer interrupted")
            force_cleanup = True
        finally:
            if interrupted_signal:
                result["interrupted_signal"] = interrupted_signal
            if process.poll() is None:
                # No process-tree-wide kill: target only identities observed
                # under this test, then the pinned tracer we launched. Its
                # --kill-on-exit handles any remaining attached tracees.
                for child in observed:
                    try:
                        if diag.signal_verified(child, signal.SIGKILL):
                            result["cleanup"].append({"pid": child.pid, "kind": "pre-exec child"})
                    except (OSError, ValueError, IndexError) as error:
                        result["observer_errors"].append("child cleanup: " + str(error))
                if parent is not None:
                    try:
                        if diag.signal_verified(parent, signal.SIGKILL):
                            result["cleanup"].append({"pid": parent.pid, "kind": "test"})
                    except (OSError, ValueError, IndexError) as error:
                        result["observer_errors"].append("test cleanup: " + str(error))
                try:
                    if tracer_fd is not None:
                        signal.pidfd_send_signal(tracer_fd, signal.SIGKILL)
                    else:
                        # Popen still owns its unreaped child: its PID cannot
                        # have been reused before poll/wait reaps it.
                        process.kill()
                    result["cleanup"].append({"pid": process.pid, "kind": "owned strace"})
                except ProcessLookupError:
                    pass
                except OSError as error:
                    result["observer_errors"].append("strace cleanup: " + str(error))
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    result["observer_errors"].append("owned strace did not reap within 1 second")
            if tracer_fd is not None:
                os.close(tracer_fd)
    result["strace_exit"] = process.returncode
    # strace forwards the command exit only when observer cleanup did not kill it.
    result["native_exit"] = process.returncode if not result["cleanup"] else None
    result["finished"] = diag.stamp()
    result["elapsed_seconds"] = result["finished"]["monotonic"] - result["started"]["monotonic"]
    diag.write_json(directory / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "linux" or not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        parser.error("Linux /proc and Python pidfd support are required")
    strace = shutil.which("strace")
    if strace is None:
        parser.error("strace with stack tracing and --kill-on-exit is required")
    asan = dict(item.split("=", 1) for item in os.environ.get("ASAN_OPTIONS", "").split(":") if "=" in item)
    if asan.get("detect_leaks") != "0":
        parser.error("the ptraced diagnostic requires explicit ASAN_OPTIONS detect_leaks=0")
    build_dir = args.build_dir.resolve(strict=True)
    argv = [str((build_dir / name).resolve(strict=True)) for name in (
        "test_bounded_child_process", "bounded_child_process_fake_child",
        "bounded_child_process_authenticated_supervisor")]
    if not all(os.access(path, os.X_OK) for path in argv):
        parser.error("the existing test and both helpers must be executable")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGINT, request_shutdown)
    summary = {"diagnostic_only": True, "result_is_acceptance": False,
               "product_deadline_unchanged": True, "ctest_timeout_unchanged": True,
               "max_attempts": ATTEMPTS, "attempts": []}
    for number in range(1, ATTEMPTS + 1):
        if interrupted_signal:
            break
        result = run_attempt(argv, build_dir, output / ("attempt-%02d" % number), strace)
        summary["attempts"].append(result)
        diag.write_json(output / "summary.json", summary)
        print("trace diagnostic attempt", number, "exit", result.get("native_exit"),
              "capture", result["capture_attempted"], "errors", result["observer_errors"], flush=True)
        if (result.get("capture", {}).get("stack_captured") or result["outer_timeout"] or
                result["observer_errors"] or interrupted_signal):
            break
    summary["stack_captured"] = any(item.get("capture", {}).get("stack_captured", False) for item in summary["attempts"])
    summary["interrupted_signal"] = interrupted_signal
    summary["test_failed_or_not_completed"] = any(item.get("native_exit") != 0 for item in summary["attempts"])
    summary["observer_error_observed"] = any(item["observer_errors"] for item in summary["attempts"])
    diag.write_json(output / "summary.json", summary)
    return int(bool(interrupted_signal) or summary["test_failed_or_not_completed"] or summary["observer_error_observed"])


if __name__ == "__main__":
    sys.exit(main())
