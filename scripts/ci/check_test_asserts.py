#!/usr/bin/env python3
"""Keep assert() and <cassert> out of tests/: test checks must survive Release builds."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


TEST_ROOT = "tests"
CXX_SUFFIXES = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".inl",
        ".ipp",
        ".ixx",
        ".tcc",
    }
)
# `#include <cassert>` and `#include "assert.h"`, with any spacing. The header
# name of a quoted include is a string literal, so the rule runs on the raw
# line; INCLUDE_DIRECTIVE_RE on the comment-free line proves that the directive
# itself is not inside a comment.
CASSERT_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"](cassert|assert\.h)[>"]')
INCLUDE_DIRECTIVE_RE = re.compile(r"#\s*include\b")
# A call of the assert macro. The lookbehind keeps static_assert, debug_assert,
# and helper names such as assert_same; a `::`-qualified `ns::assert(` cannot
# reach the macro either. Comments are blanked before matching, so whitespace
# also covers an assert/* comment */(...) spelling and a line continuation.
ASSERT_CALL_RE = re.compile(r"(?<![A-Za-z0-9_:])assert(?:\s|\\\n)*\(")
ASSERT_MESSAGE = (
    "assert() is compiled out when NDEBUG is defined; use TEST_ASSERT from "
    "tests/support/test_assert.hpp"
)
INCLUDE_MESSAGE = (
    "do not include <cassert> or <assert.h> in tests; include "
    '"support/test_assert.hpp" and use TEST_ASSERT'
)


class ContractError(RuntimeError):
    """The test tree could not be scanned safely."""


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    message: str


def _blank(text: str) -> str:
    """Preserve newlines and offsets while hiding comments and literals."""
    return "".join("\n" if char == "\n" else " " for char in text)


def strip_comments_and_literals(text: str, relative: str) -> str:
    """Return C/C++ code with comments and literals replaced by whitespace."""
    output: list[str] = []
    index = 0
    size = len(text)

    def line_at(offset: int) -> int:
        return text.count("\n", 0, offset) + 1

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
                raise ContractError(
                    f"{relative}:{line_at(index)}: unterminated block comment"
                )
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
                        raise ContractError(
                            f"{relative}:{line_at(index)}: unterminated raw string literal"
                        )
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
                        f"{relative}:{line_at(index)}: unterminated string or character literal"
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
                    f"{relative}:{line_at(index)}: unterminated string or character literal"
                )
            output.append(_blank(text[index:end]))
            index = end
            continue

        output.append(char)
        index += 1

    return "".join(output)


def iter_test_sources(root: Path) -> list[Path]:
    files: list[Path] = []

    def fail_walk(error: OSError) -> None:
        raise ContractError(f"cannot traverse the test tree: {error}")

    test_root = root / TEST_ROOT
    if not test_root.is_dir():
        raise ContractError(f"{TEST_ROOT}: test source root is missing")
    for directory, directory_names, file_names in os.walk(
        test_root, followlinks=False, onerror=fail_walk
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
            if candidate.suffix.lower() not in CXX_SUFFIXES:
                continue
            if candidate.is_symlink():
                raise ContractError(f"{relative}: source symlinks are not scanned")
            if not candidate.is_file():
                raise ContractError(f"{relative}: unsupported non-regular source entry")
            files.append(candidate)
    if not files:
        raise ContractError(f"{TEST_ROOT}: no C/C++ test sources found")
    return sorted(files)


def _read(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ContractError(f"{relative}: cannot read UTF-8 source: {error}") from error
    if "\0" in text:
        raise ContractError(f"{relative}: NUL byte in test source")
    return text


def scan(root: Path) -> list[Violation]:
    violations: list[Violation] = []
    for path in iter_test_sources(root):
        relative = path.relative_to(root).as_posix()
        text = _read(path, root)
        code = strip_comments_and_literals(text, relative)
        for offset, (raw, line) in enumerate(zip(text.split("\n"), code.split("\n")), start=1):
            if CASSERT_INCLUDE_RE.match(raw) and INCLUDE_DIRECTIVE_RE.search(line):
                violations.append(Violation(relative, offset, INCLUDE_MESSAGE))
        for match in ASSERT_CALL_RE.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            violations.append(Violation(relative, line, ASSERT_MESSAGE))
    return sorted(violations)


def _run_cli(root: Path) -> tuple[int, str]:
    """Run this script the way CI does and return its exit status and output."""
    completed = subprocess.run(
        [sys.executable, os.fspath(Path(__file__).resolve()), "--root", os.fspath(root)],
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.returncode, completed.stdout + completed.stderr


def _expect_contract_error(label: str, action) -> None:
    try:
        action()
    except ContractError:
        return
    raise ContractError(f"self-test did not fail closed on {label}")


def run_self_test() -> None:
    """Exercise comment and literal handling, both rules, and fail-closed paths."""
    with tempfile.TemporaryDirectory(prefix="gnfs-test-asserts-") as temp_directory:
        root = Path(temp_directory)

        def write(relative: str, *lines: str) -> Path:
            target = root / TEST_ROOT / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return target

        # Comments, literals, digit separators, static_assert, helper names,
        # and a `::`-qualified assert() are not uses of the macro.
        write(
            "clean.cpp",
            "// assert(x) and #include <cassert> are fine inside a comment.",
            "/* assert(x)",
            "   #include <cassert> */",
            "/* not a directive */ #include <string_view>",
            '#include "other.hpp" // trailing #include <cassert>',
            '#include "support/test_assert.hpp"',
            "",
            "#include <string>",
            "#include <type_traits>",
            "",
            "namespace {",
            "",
            "void assert_same(int a, int b) {",
            "    TEST_ASSERT(a == b);",
            "}",
            "",
            "void debug_assert_impl(bool condition) {",
            "    TEST_ASSERT(condition);",
            "}",
            "",
            'const char* quoted = "assert(x) #include <cassert>";',
            'const char* raw = R"x(assert(x) #include <cassert>)x";',
            "auto separated = 1'000'000;",
            "auto character = '\"';",
            "",
            "} // namespace",
            "",
            "static_assert(std::is_same_v<int, int>, \"static_assert stays\");",
            "",
            "int case_one() {",
            "    int value = 1;",
            "    TEST_ASSERT(value == 1);",
            "    assert_same(1, 1);",
            "    debug_assert_impl(true);",
            "    return value;",
            "}",
        )
        write(
            "qualified.cpp",
            "namespace other {",
            "int verify(bool condition);",
            "}",
            "int case_two(bool flag) {",
            "    other::assert(flag);",
            "    return verify(flag);",
            "}",
        )
        write("fixtures/nested/clean.hpp", "#pragma once", "static_assert(sizeof(int) == 4);")
        # Files without a C/C++ suffix are never scanned.
        write("notes.txt", "assert(1 == 2)", "#include <cassert>")
        write("not_source.py", "def helper():", "    assert False")

        if scan(root):
            raise ContractError("self-test rejected comments, literals, or helper names")

        call = write(
            "assert_call.cpp",
            '#include "support/test_assert.hpp"',
            "",
            "int f(int x) {",
            "    assert(x > 0);",
            "    return x;",
            "}",
        )
        spaced = write(
            "assert_spaced.cpp",
            "int g(int x) {",
            "    assert /* why */ (x > 0);",
            "    return x;",
            "}",
        )
        continued = write("assert_continued.cpp", "int h(int x) {", "    assert\\", "        (x);")
        angle = write("cassert_angle.cpp", "#include <cassert>", "int i() { return 0; }")
        quoted = write("cassert_quoted.cpp", '#  include "cassert"', "int j() { return 0; }")
        legacy = write("assert_h.cpp", "#include <assert.h>", "int k() { return 0; }")
        header = write(
            "fixtures/legacy.hpp",
            "#pragma once",
            "#include <assert.h>",
            "inline int l(int x) {",
            "    assert(x);",
            "    return x;",
            "}",
        )
        expected = [
            Violation("tests/assert_call.cpp", 4, ASSERT_MESSAGE),
            Violation("tests/assert_continued.cpp", 2, ASSERT_MESSAGE),
            Violation("tests/assert_h.cpp", 1, INCLUDE_MESSAGE),
            Violation("tests/assert_spaced.cpp", 2, ASSERT_MESSAGE),
            Violation("tests/cassert_angle.cpp", 1, INCLUDE_MESSAGE),
            Violation("tests/cassert_quoted.cpp", 1, INCLUDE_MESSAGE),
            Violation("tests/fixtures/legacy.hpp", 2, INCLUDE_MESSAGE),
            Violation("tests/fixtures/legacy.hpp", 4, ASSERT_MESSAGE),
        ]
        observed = scan(root)
        if observed != expected:
            raise ContractError(f"self-test expected {expected}, found {observed}")

        # The command line names every violation and passes only a clean tree.
        status, output = _run_cli(root)
        if status != 1 or "[PASS]" in output:
            raise ContractError("self-test accepted assert() on the command line")
        for entry in expected:
            if f"{entry.path}:{entry.line}: {entry.message}" not in output:
                raise ContractError(
                    f"self-test lost {entry.path}:{entry.line} on the command line"
                )
        for path in (call, spaced, continued, angle, quoted, legacy, header):
            path.unlink()
        status, output = _run_cli(root)
        if status != 0 or "[PASS]" not in output:
            raise ContractError(f"self-test rejected a clean tree: {output.strip()}")

        # Malformed sources fail closed, on the command line as well.
        broken = write("broken.cpp", 'const char* text = "unterminated;\n')
        _expect_contract_error("an unterminated string", lambda: scan(root))
        broken.write_text("/* unterminated\n", encoding="utf-8")
        _expect_contract_error("an unterminated comment", lambda: scan(root))
        status, output = _run_cli(root)
        if status != 1 or "unterminated" not in output:
            raise ContractError("self-test accepted a malformed source on the command line")
        broken.write_text('const char* raw = R"x(unterminated\n', encoding="utf-8")
        _expect_contract_error("an unterminated raw string", lambda: scan(root))
        broken.write_bytes(b"int nul\x00value;\n")
        _expect_contract_error("a NUL byte", lambda: scan(root))
        broken.write_bytes(b"int bad = '\xff';\n")
        _expect_contract_error("invalid UTF-8", lambda: scan(root))
        broken.unlink()

        # Symlinks are refused where the host can create them.
        for target, link in (("notes.txt", "linked.cpp"), ("fixtures", "linked_dir")):
            link_path = root / TEST_ROOT / link
            try:
                os.symlink(target, link_path)
            except (OSError, NotImplementedError):
                continue
            _expect_contract_error(f"a symlink ({link})", lambda: scan(root))
            link_path.unlink()

        # An empty or missing test tree is not a pass.
        for path in sorted((root / TEST_ROOT).rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
        _expect_contract_error("an empty test tree", lambda: scan(root))
        (root / TEST_ROOT).rename(root / "moved_tests")
        _expect_contract_error("a missing test tree", lambda: scan(root))


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
                print("[PASS] test assertion checker self-test")
            return 0

        root = (args.root or Path(__file__).resolve().parents[2]).resolve()
        violations = scan(root)
    except ContractError as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    if violations:
        for violation in violations:
            print(
                f"[FAIL] {violation.path}:{violation.line}: {violation.message}",
                file=sys.stderr,
            )
        return 1

    if not args.quiet:
        print(
            f"[PASS] no assert() or <cassert> in the C/C++ sources under {TEST_ROOT}/; "
            "test checks stay active in Release builds"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
