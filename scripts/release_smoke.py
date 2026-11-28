#!/usr/bin/env python3
"""Factor three fixed inputs with a release CLI through the event-stream protocol.

A release binary that reports the right ``--version`` can still fail to
factor, and the ordinary CI test binaries cannot show otherwise because they
use another toolchain and build configuration. This script runs each case in
a child process with ``--event-stream`` and checks the exit status, the JSON
Lines protocol, and the returned factors.

It needs only the Python standard library and stays within Python 3.8, the
version in the Ubuntu 20.04 release container, so the same file runs there,
in the MSYS2 UCRT64 shell, and on macOS.
"""

from __future__ import annotations

import argparse
from contextlib import redirect_stderr
from dataclasses import dataclass
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import IO, Sequence
from unittest.mock import patch


SCHEMA_VERSION = 1
# ``--lang en`` keeps diagnostic text in English without changing the route.
EVENT_STREAM_ARGUMENTS = ("--event-stream", "--lang", "en")
# Both variables change the default route in Pipeline::select_method, so every
# case removes them before applying its own route environment.
ROUTE_ENVIRONMENT = ("GNFS_FORCE_SIQS", "GNFS_DISABLE_SIQS")
PREVIEW_LIMIT = 2000
FAKE_MODE_VARIABLE = "RELEASE_SMOKE_FAKE_MODE"


@dataclass(frozen=True)
class Case:
    """One factorization request and the result the release CLI must produce."""

    name: str
    number: str
    factors: tuple[str, ...]
    arguments: tuple[str, ...] = ()
    environment: tuple[tuple[str, str], ...] = ()
    # None leaves the algorithm unchecked. The other cases name the algorithm
    # that must produce the split (``stats.executed_algorithm``).
    executed_algorithm: str | None = None
    # ``--complete`` results must prove a complete prime factorization. The
    # single-split API leaves both flags false by contract.
    require_complete: bool = False
    # Standard error is diagnostic text in the event-stream protocol. The
    # pipeline passes verbose=true to siqs::factor (src/api/pipeline.cpp), so
    # the SIQS path always prints progress lines there. Only the case that
    # mirrors the Workbench package probe requires empty standard error.
    require_empty_stderr: bool = False

    @property
    def sorted_factors(self) -> list[str]:
        return sorted(self.factors, key=int)


CASES = (
    Case(
        name="360-complete",
        number="360",
        factors=("2", "2", "2", "3", "3", "5"),
        arguments=("--complete",),
        require_complete=True,
        require_empty_stderr=True,
    ),
    Case(
        name="30-digit-siqs",
        number="191890662872264280268190961847",
        factors=("427415244035077", "448956057488011"),
        executed_algorithm="siqs",
    ),
    Case(
        name="25-digit-forced-gnfs",
        number="1669994516749619561652133",
        factors=("1292282676071", "1292282677523"),
        arguments=("--method", "gnfs"),
        # Without this variable the GNFS route first spends up to 30 seconds on
        # an SIQS probe for inputs of at most 100 digits.
        environment=(("GNFS_DISABLE_SIQS", "1"),),
        executed_algorithm="gnfs",
    ),
)


# Scripted stand-in for the GNFS CLI. ``--self-test`` writes it to a temporary
# directory and runs it with the current interpreter, which needs no launcher
# file on any platform. It validates the exact invocation, then behaves as the
# mode named in RELEASE_SMOKE_FAKE_MODE requests.
FAKE_CLI_SOURCE = '''#!/usr/bin/env python3
"""Scripted stand-in for the GNFS CLI; only release_smoke.py --self-test runs it."""

import json
import os
import sys
import time

NUMBERS = {
    "360": (["2", "2", "2", "3", "3", "5"], "trial"),
    "191890662872264280268190961847": (["427415244035077", "448956057488011"], "siqs"),
    "1669994516749619561652133": (["1292282676071", "1292282677523"], "gnfs"),
}
EXPECTED_ARGUMENTS = {
    "360": ["--event-stream", "--lang", "en", "--complete"],
    "191890662872264280268190961847": ["--event-stream", "--lang", "en"],
    "1669994516749619561652133": ["--event-stream", "--lang", "en", "--method", "gnfs"],
}
FORCED_GNFS = "1669994516749619561652133"

mode = os.environ.get("RELEASE_SMOKE_FAKE_MODE", "")
arguments = sys.argv[1:]
number = arguments[0] if arguments else ""
if number not in NUMBERS or arguments[1:] != EXPECTED_ARGUMENTS[number]:
    sys.exit(3)
disabled = os.environ.get("GNFS_DISABLE_SIQS")
forced = os.environ.get("GNFS_FORCE_SIQS")
expected_disabled = "1" if number == FORCED_GNFS else None
if disabled != expected_disabled or forced is not None:
    sys.exit(4)
if mode == "timeout" and number == FORCED_GNFS:
    time.sleep(60)
if mode == "stderr":
    sys.stderr.write("fake diagnostic output\\n")


def emit(event):
    newline = "\\r\\n" if mode == "crlf" else "\\n"
    print(json.dumps(event, separators=(",", ":")) + newline, end="", flush=True)


complete = "--complete" in arguments
factors, algorithm = NUMBERS[number]
if mode == "wrong_factors":
    factors[0] = str(int(factors[0]) + 1)
if mode == "wrong_algorithm" and algorithm != "trial":
    algorithm = "gnfs" if algorithm == "siqs" else "siqs"

emit({"schema_version": 1, "type": "started", "n": number})
if mode == "invalid_json":
    print('{"schema_version":1,"type":"progress"', flush=True)
if mode == "non_finite":
    print('{"schema_version":1,"type":"progress","phase_progress":NaN}', flush=True)
emit({
    "schema_version": 2 if mode == "wrong_schema" else 1,
    "type": "progress",
    "phase": "extract",
    "message": "fake progress",
})
result = {
    "success": mode != "unsuccessful",
    "factorization_complete": complete and mode != "incomplete_flags",
    "factors_prime": complete and mode != "incomplete_flags",
    "n": str(int(number) + 1) if mode == "wrong_input" else number,
    "n_bits": int(number).bit_length(),
    "n_digits": len(number),
    "method": algorithm,
    "method_name": algorithm.upper(),
    "factors": factors,
    "timings": {"non_gnfs_s": 0.0, "total_s": 0.0},
    "stats": {
        "executed_algorithm": algorithm,
        "algorithms_executed": [algorithm],
        "gnfs_parameters_applied": algorithm == "gnfs",
    },
    "attempts": [],
}
event = {"schema_version": 1, "type": "result", "result": result}
if mode != "missing_result":
    emit(event)
    if mode == "extra_result":
        emit(event)
if mode == "unterminated":
    print(json.dumps(event, separators=(",", ":")), end="", flush=True)
sys.exit(3 if mode == "nonzero_exit" else 0)
'''


def _ascii(text: str) -> str:
    """Escape non-ASCII characters so diagnostics print under any console code page."""
    return text.encode("ascii", "backslashreplace").decode("ascii")


def _emit(stream: IO[str], text: str) -> None:
    print(_ascii(text), file=stream, flush=True)


def _clip(text: str) -> str:
    if len(text) <= PREVIEW_LIMIT:
        return repr(text)
    half = PREVIEW_LIMIT // 2
    return f"{text[:half]!r} ... {text[-half:]!r}"


def _decode(payload: bytes | None) -> str:
    return (payload or b"").decode("utf-8", errors="replace")


def _split_lines(text: str) -> list[str]:
    """Split on LF and keep interior empty lines, which are never valid events."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def _reject_constant(name: str) -> None:
    raise ValueError(f"{name} is not valid JSON")


def _describe_exit(code: int) -> str:
    if code < 0:
        return f"{code} (terminated by signal {-code})"
    if code > 255:
        # Windows statuses such as 0xC0000135 (a DLL was not found).
        return f"{code} (0x{code & 0xFFFFFFFF:08X})"
    return str(code)


def _sorted_numbers(values: object) -> list[str] | None:
    """Sort decimal strings numerically, or return None for anything else."""
    if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
        return None
    try:
        return sorted(values, key=int)
    except ValueError:
        return None


def _child_environment(case: Case) -> dict[str, str]:
    environment = dict(os.environ)
    for name in ROUTE_ENVIRONMENT:
        environment.pop(name, None)
    environment.update(case.environment)
    return environment


def _check_fixtures() -> None:
    for case in CASES:
        if math.prod(int(factor) for factor in case.factors) != int(case.number):
            raise ValueError(f"fixture {case.name}: its factors must multiply to its input")


def _examine(case: Case, stdout: str, stderr: str, stderr_bytes: int) -> list[str]:
    """Return every way the captured output differs from the expected protocol."""
    problems = []
    result_events = []
    for index, line in enumerate(_split_lines(stdout), start=1):
        try:
            event = json.loads(line, parse_constant=_reject_constant)
            if not isinstance(event, dict):
                raise ValueError("expected one JSON object")
        except ValueError as error:
            problems.append(
                f"stdout line {index}: expected one JSON object, actual {_clip(line)} ({error})"
            )
            continue
        if event.get("schema_version") != SCHEMA_VERSION:
            problems.append(
                f"stdout line {index}: schema_version: expected {SCHEMA_VERSION}, "
                f"actual {event.get('schema_version')!r}"
            )
        if event.get("type") == "result":
            result_events.append(event)
    if stdout and not stdout.endswith("\n"):
        problems.append(
            "stdout: expected every event to end with a newline, "
            "actual last event is unterminated"
        )
    if len(result_events) != 1:
        problems.append(f"result events: expected exactly 1, actual {len(result_events)}")
        return problems

    result = result_events[0].get("result")
    if not isinstance(result, dict):
        problems.append(f"result payload: expected a JSON object, actual {result!r}")
        return problems
    if result.get("success") is not True:
        problems.append(f"success: expected true, actual {result.get('success')!r}")
    if result.get("n") != case.number:
        problems.append(f"n: expected {case.number}, actual {result.get('n')!r}")
    reported = result.get("factors")
    actual_factors = _sorted_numbers(reported)
    if actual_factors != case.sorted_factors:
        problems.append(
            f"factors: expected {case.sorted_factors}, actual "
            f"{actual_factors if actual_factors is not None else reported!r}"
        )
    if case.executed_algorithm is not None:
        stats = result.get("stats")
        actual_algorithm = stats.get("executed_algorithm") if isinstance(stats, dict) else None
        if actual_algorithm != case.executed_algorithm:
            problems.append(
                f"executed_algorithm: expected {case.executed_algorithm!r}, "
                f"actual {actual_algorithm!r}"
            )
    if case.require_complete:
        for flag in ("factorization_complete", "factors_prime"):
            if result.get(flag) is not True:
                problems.append(f"{flag}: expected true, actual {result.get(flag)!r}")
    if case.require_empty_stderr and stderr:
        problems.append(
            f"stderr: expected no output, actual {stderr_bytes} bytes: {_clip(stderr)}"
        )
    return problems


def _as_bytes(payload: object) -> bytes:
    # TimeoutExpired carries the output read so far as bytes, text, or None.
    if isinstance(payload, bytes):
        return payload
    if isinstance(payload, str):
        return payload.encode("utf-8", errors="replace")
    return b""


def _report_failure(
    stream: IO[str],
    case: Case,
    arguments: Sequence[str],
    elapsed: float,
    problems: Sequence[str],
    stdout: bytes,
    stderr: bytes,
) -> None:
    _emit(stream, f"release smoke: {case.name} FAILED ({elapsed:.2f}s)")
    for problem in problems:
        _emit(stream, f"release smoke:   {problem}")
    _emit(stream, f"release smoke:   command: {' '.join(arguments)}")
    _emit(stream, f"release smoke:   stdout ({len(stdout)} bytes): {_clip(_decode(stdout))}")
    _emit(stream, f"release smoke:   stderr ({len(stderr)} bytes): {_clip(_decode(stderr))}")


def run_case(command: Sequence[str], case: Case, timeout: float, stream: IO[str]) -> bool:
    """Run one case and print either its ok line or every observed difference."""
    arguments = [*command, case.number, *EVENT_STREAM_ARGUMENTS, *case.arguments]
    started = time.monotonic()
    try:
        completed = subprocess.run(
            arguments,
            check=False,
            capture_output=True,
            env=_child_environment(case),
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        problems = [
            f"completion: expected the process to exit within {timeout:g} seconds, "
            "actual no exit before the deadline (child killed)"
        ]
        _report_failure(
            stream,
            case,
            arguments,
            time.monotonic() - started,
            problems,
            _as_bytes(error.stdout),
            _as_bytes(error.stderr),
        )
        return False
    except OSError as error:
        problems = [f"start: expected the executable to launch, actual {error}"]
        _report_failure(
            stream, case, arguments, time.monotonic() - started, problems, b"", b""
        )
        return False

    elapsed = time.monotonic() - started
    stdout = _decode(completed.stdout)
    stderr = _decode(completed.stderr)
    problems = []
    if completed.returncode != 0:
        problems.append(
            f"exit code: expected 0, actual {_describe_exit(completed.returncode)}"
        )
    problems.extend(_examine(case, stdout, stderr, len(completed.stderr)))
    if problems:
        _report_failure(
            stream,
            case,
            arguments,
            elapsed,
            problems,
            completed.stdout,
            completed.stderr,
        )
        return False

    _emit(stream, f"release smoke: {case.name} ok ({elapsed:.2f}s)")
    if completed.stderr:
        stderr_lines = stderr.strip().splitlines()
        _emit(
            stream,
            f"release smoke: {case.name} stderr: {len(completed.stderr)} bytes of "
            f"diagnostics, which this case does not assert; last line: "
            f"{_clip(stderr_lines[-1] if stderr_lines else '')}",
        )
    return True


def run_cases(command: Sequence[str], timeout: float, stream: IO[str]) -> bool:
    """Run every case in order and return True when all of them passed."""
    _check_fixtures()
    _emit(stream, f"release smoke: executable: {' '.join(str(part) for part in command)}")
    # The Windows job runs this script with PATH reduced to the system
    # directories, so this line records the search path the children inherit.
    _emit(stream, f"release smoke: child PATH: {os.environ.get('PATH', '')}")
    failed = [case.name for case in CASES if not run_case(command, case, timeout, stream)]
    summary = f"release smoke: {len(CASES) - len(failed)} of {len(CASES)} cases passed"
    if failed:
        summary += f"; failed: {', '.join(failed)}"
    _emit(stream, summary)
    return not failed


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--executable", help="path to the gnfs CLI to exercise")
    target.add_argument(
        "--self-test",
        action="store_true",
        help="check this script against a scripted fake CLI and exit",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="seconds allowed per case (default: 300)",
    )
    arguments = parser.parse_args(argv)
    if not math.isfinite(arguments.timeout) or arguments.timeout <= 0:
        parser.error("--timeout must be a positive finite number of seconds")
    return arguments


def _accepts(arguments: Sequence[str]) -> bool:
    try:
        parse_arguments(arguments)
    except SystemExit:
        return False
    return True


def _rejects(arguments: Sequence[str]) -> bool:
    with redirect_stderr(io.StringIO()):
        try:
            parse_arguments(arguments)
        except SystemExit as error:
            return error.code == 2
    return False


def self_test(stream: IO[str] | None = None) -> bool:
    """Prove that the checker accepts a conforming CLI and rejects each fault."""
    stream = stream or sys.stdout
    all_names = [case.name for case in CASES]
    # (fake mode, seconds per case, cases that must fail, text the output must contain)
    scenarios = (
        ("", 30.0, [], ""),
        ("crlf", 30.0, [], ""),
        ("wrong_factors", 30.0, all_names, "factors: expected"),
        ("wrong_input", 30.0, all_names, "n: expected"),
        ("nonzero_exit", 30.0, all_names, "exit code: expected 0, actual 3"),
        ("stderr", 30.0, ["360-complete"], "stderr: expected no output"),
        ("extra_result", 30.0, all_names, "result events: expected exactly 1, actual 2"),
        ("unterminated", 30.0, all_names, "actual last event is unterminated"),
        ("invalid_json", 30.0, all_names, "expected one JSON object"),
        ("non_finite", 30.0, all_names, "NaN is not valid JSON"),
        ("wrong_schema", 30.0, all_names, "schema_version: expected 1, actual 2"),
        ("missing_result", 30.0, all_names, "result events: expected exactly 1, actual 0"),
        ("wrong_algorithm", 30.0, all_names[1:], "executed_algorithm: expected 'siqs'"),
        ("incomplete_flags", 30.0, ["360-complete"], "factorization_complete: expected true"),
        ("unsuccessful", 30.0, all_names, "success: expected true"),
        ("timeout", 3.0, ["25-digit-forced-gnfs"], "actual no exit before the deadline"),
    )
    failures = []
    with tempfile.TemporaryDirectory(prefix="gnfs-release-smoke-") as directory:
        fake = Path(directory) / "fake_gnfs_cli.py"
        fake.write_text(FAKE_CLI_SOURCE, encoding="utf-8")
        command = (sys.executable, str(fake))
        # The route variables of the calling environment must not leak into a case.
        polluted = {"GNFS_FORCE_SIQS": "1", "GNFS_DISABLE_SIQS": "1"}
        for mode, timeout, expected_failed, fragment in scenarios:
            output = io.StringIO()
            mode_environment = {FAKE_MODE_VARIABLE: mode} if mode else {}
            with patch.dict(os.environ, {**polluted, **mode_environment}, clear=False):
                passed = run_cases(command, timeout, output)
            text = output.getvalue()
            failed = re.findall(r"^release smoke: (\S+) FAILED", text, flags=re.MULTILINE)
            passed_cases = re.findall(
                r"^release smoke: (\S+) ok \(\d+\.\d\d+s\)$", text, flags=re.MULTILINE
            )
            expected_passed = [name for name in all_names if name not in expected_failed]
            if (
                passed != (not expected_failed)
                or failed != expected_failed
                or passed_cases != expected_passed
                or fragment not in text
            ):
                failures.append(
                    f"fake mode {mode or 'none'}: expected failures {expected_failed} "
                    f"containing {fragment!r}, actual output:\n{text}"
                )
    for arguments in (
        [],
        ["--executable", "gnfs", "--self-test"],
        ["--executable", "gnfs", "--timeout", "0"],
        ["--executable", "gnfs", "--timeout", "-5"],
        ["--executable", "gnfs", "--timeout", "nan"],
        ["--executable", "gnfs", "--timeout", "inf"],
    ):
        if not _rejects(arguments):
            failures.append(f"argument parser accepted {arguments}")
    for arguments, timeout in (
        (["--executable", "gnfs"], 300.0),
        (["--executable", "gnfs", "--timeout", "12"], 12.0),
        (["--self-test"], 300.0),
    ):
        if not _accepts(arguments) or parse_arguments(arguments).timeout != timeout:
            failures.append(f"argument parser rejected {arguments}")

    if failures:
        for failure in failures:
            _emit(stream, f"release smoke self-test: FAIL {failure}")
        return False
    _emit(stream, "release smoke self-test: PASS")
    return True


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    if arguments.self_test:
        return 0 if self_test() else 1

    executable = Path(arguments.executable).resolve()
    if not executable.is_file():
        _emit(sys.stdout, f"release smoke: executable does not exist: {executable}")
        return 1
    if os.name != "nt" and not os.access(executable, os.X_OK):
        _emit(sys.stdout, f"release smoke: executable is not runnable: {executable}")
        return 1
    return 0 if run_cases([str(executable)], arguments.timeout, sys.stdout) else 1


if __name__ == "__main__":
    raise SystemExit(main())
