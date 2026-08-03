#!/usr/bin/env python3
"""Reject direct stdout writes outside the GNFS CLI ownership boundary."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import sys
import tempfile


PRODUCTION_ROOTS = ("include", "src")
STDOUT_OWNER = "src/cli/main.cpp"
FORBIDDEN_SINKS = (
    ("C++ stdout stream", re.compile(r"\b(?:std\s*::\s*)?(?:cout|wcout)\b")),
    (
        "C stdout function",
        re.compile(r"\b(?:printf|puts|putchar|vprintf|wprintf|putwchar|vwprintf)\s*\("),
    ),
    (
        "C stdout handle write",
        re.compile(
            r"\b(?:fprintf|vfprintf|fputs|fputc|fwrite|fwprintf|vfwprintf|fputws|fputwc)"
            r"\s*\([^;{}]*\bstdout\b"
        ),
    ),
    ("C++23 stdout function", re.compile(r"\bstd\s*::\s*(?:print|println)\s*\(")),
    (
        "POSIX stdout write",
        re.compile(r"\b_?write\s*\(\s*(?:1|STDOUT_FILENO)\s*,"),
    ),
)


class ContractError(RuntimeError):
    """A source tree could not be scanned safely."""


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    sink: str


def _blank(text: str) -> str:
    """Preserve newlines and offsets while hiding comments and literals."""
    return "".join("\n" if char == "\n" else " " for char in text)


def strip_comments_and_literals(text: str, relative: str) -> str:
    """Return C/C++ code with comments and literals replaced by whitespace."""
    output: list[str] = []
    index = 0
    size = len(text)

    while index < size:
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            if end < 0:
                output.append(_blank(text[index:]))
                break
            output.append(_blank(text[index:end]))
            index = end
            continue

        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise ContractError(f"{relative}: unterminated block comment")
            end += 2
            output.append(_blank(text[index:end]))
            index = end
            continue

        if text.startswith('R"', index):
            delimiter_end = text.find("(", index + 2, index + 19)
            if delimiter_end >= 0:
                delimiter = text[index + 2 : delimiter_end]
                if not any(char.isspace() or char in "\\()" for char in delimiter):
                    terminator = ")" + delimiter + '"'
                    end = text.find(terminator, delimiter_end + 1)
                    if end < 0:
                        raise ContractError(f"{relative}: unterminated raw string literal")
                    end += len(terminator)
                    output.append(_blank(text[index:end]))
                    index = end
                    continue

        char = text[index]
        if char in {'"', "'"}:
            # A quote between numeric-token characters is a C++ digit separator.
            if (
                char == "'"
                and index > 0
                and index + 1 < size
                and (text[index - 1].isdigit() or text[index - 1].lower() in "abcdef")
                and text[index + 1].isalnum()
            ):
                output.append(char)
                index += 1
                continue

            quote = char
            end = index + 1
            escaped = False
            while end < size:
                current = text[end]
                if current == "\n" and not escaped:
                    raise ContractError(
                        f"{relative}:{text.count(chr(10), 0, index) + 1}: "
                        "unterminated string or character literal"
                    )
                if current == quote and not escaped:
                    end += 1
                    break
                if current == "\\" and not escaped:
                    escaped = True
                else:
                    escaped = False
                end += 1
            else:
                raise ContractError(
                    f"{relative}:{text.count(chr(10), 0, index) + 1}: "
                    "unterminated string or character literal"
                )
            output.append(_blank(text[index:end]))
            index = end
            continue

        output.append(char)
        index += 1

    return "".join(output)


def iter_production_files(root: Path) -> list[Path]:
    files: list[Path] = []

    def fail_walk(error: OSError) -> None:
        raise ContractError(f"cannot traverse production source tree: {error}")

    for relative_root in PRODUCTION_ROOTS:
        source_root = root / relative_root
        if not source_root.is_dir():
            raise ContractError(f"{relative_root}: production source root is missing")
        for directory, directory_names, file_names in os.walk(
            source_root, followlinks=False, onerror=fail_walk
        ):
            current = Path(directory)
            for name in directory_names:
                candidate = current / name
                if candidate.is_symlink():
                    raise ContractError(
                        f"{candidate.relative_to(root).as_posix()}: directory symlinks are not scanned"
                    )
            for name in file_names:
                candidate = current / name
                relative = candidate.relative_to(root).as_posix()
                if candidate.is_symlink():
                    raise ContractError(f"{relative}: source symlinks are not scanned")
                if not candidate.is_file():
                    raise ContractError(f"{relative}: unsupported non-regular source entry")
                files.append(candidate)

    owner = root / STDOUT_OWNER
    if owner not in files:
        raise ContractError(f"{STDOUT_OWNER}: stdout owner is missing from the source inventory")
    return sorted(files)


def scan(root: Path) -> list[Violation]:
    violations: list[Violation] = []
    for path in iter_production_files(root):
        relative = path.relative_to(root).as_posix()
        if relative == STDOUT_OWNER:
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise ContractError(f"{relative}: cannot read UTF-8 source: {error}") from error
        if "\0" in source:
            raise ContractError(f"{relative}: NUL byte in production source")

        code = strip_comments_and_literals(source, relative)
        for sink, pattern in FORBIDDEN_SINKS:
            for match in pattern.finditer(code):
                violations.append(
                    Violation(relative, code.count("\n", 0, match.start()) + 1, sink)
                )
    return sorted(violations)


def run_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="gnfs-stdout-contract-") as temp_directory:
        root = Path(temp_directory)
        (root / "include").mkdir()
        (root / "src" / "cli").mkdir(parents=True)
        (root / STDOUT_OWNER).write_text("std::cout << value;\n", encoding="utf-8")
        sample = root / "src" / "sample.cpp"
        sample.write_text(
            '// std::cout << "comment";\n'
            'const char* text = "printf(\\"literal\\")";\n'
            "auto n = 1'000;\n"
            "std::cerr << text << n;\n",
            encoding="utf-8",
        )
        if scan(root):
            raise ContractError("self-test rejected comments, literals, or stderr")

        forbidden_samples = (
            "std::cout << value;\n",
            "std::wcout << value;\n",
            'wprintf(L"value");\n',
            'fputws(L"value", stdout);\n',
        )
        for forbidden in forbidden_samples:
            sample.write_text(forbidden, encoding="utf-8")
            violations = scan(root)
            if len(violations) != 1 or violations[0].line != 1:
                raise ContractError(f"self-test failed to detect {forbidden.strip()}")

        sample.write_text("/* unterminated", encoding="utf-8")
        try:
            scan(root)
        except ContractError:
            pass
        else:
            raise ContractError("self-test did not fail closed on malformed source")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quiet", action="store_true", help="print only failures")
    parser.add_argument("--root", type=Path, help="repository root (defaults to script parent)")
    parser.add_argument("--self-test", action="store_true", help="run checker regression tests")
    args = parser.parse_args()

    try:
        if args.self_test:
            run_self_test()
            if not args.quiet:
                print("[PASS] stdout contract checker self-test")
            return 0

        root = (args.root or Path(__file__).resolve().parents[1]).resolve()
        violations = scan(root)
    except ContractError as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    if violations:
        for violation in violations:
            print(
                f"[FAIL] {violation.path}:{violation.line}: {violation.sink} is reserved for "
                f"{STDOUT_OWNER}",
                file=sys.stderr,
            )
        return 1

    if not args.quiet:
        print(f"[PASS] direct stdout writes are confined to {STDOUT_OWNER}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
