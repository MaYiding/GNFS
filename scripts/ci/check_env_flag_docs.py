#!/usr/bin/env python3
"""Keep GNFS_* names from source string literals documented under docs/env-flags."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


SOURCE_ROOTS = ("include", "src", "apps")
DOC_ROOT = "docs/env-flags"
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
        ".m",
        ".mm",
        ".tcc",
    }
)
SWIFT_SUFFIXES = frozenset({".swift"})
# A name is the maximal run of upper-case letters, digits, and underscores that
# follows "GNFS_". The lookarounds reject identifiers that merely contain
# "GNFS_" and names that continue in lower case, so a match is exactly the
# token a reader would quote from the source.
NAME_RE = re.compile(r"(?<![A-Za-z0-9_])GNFS_[A-Z0-9_]+(?![A-Za-z0-9_])")
CXX_STRING_PREFIXES = frozenset({"u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"})


class ContractError(RuntimeError):
    """A source tree could not be scanned safely."""


@dataclass(frozen=True, order=True)
class Exemption:
    """A collected name that must not get a runtime flag page."""

    name: str
    reason: str


# Names that appear in string literals without being runtime tuning switches.
# Every entry needs a reason, and an entry fails the check once its name
# disappears from the sources, so the table cannot drift.
EXEMPTIONS: tuple[Exemption, ...] = (
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_",
        "test hook: GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_<chunk_id> makes one "
        "distributed-sieve worker attempt exit with a failure",
    ),
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_HANG_ATTEMPT_",
        "test hook: GNFS_DISTRIBUTED_SIEVE_HANG_ATTEMPT_<chunk_id> makes one "
        "worker attempt hang so the parent watchdog has to reap it",
    ),
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_",
        "test hook: GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_<chunk_id> "
        "stops a worker after the cleanup handoff is published",
    ),
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_",
        "test hook: GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_<chunk_id> "
        "corrupts the worker completion report",
    ),
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_",
        "test hook: GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_<chunk_id> "
        "corrupts the worker sequence receipt",
    ),
    Exemption(
        "GNFS_DISTRIBUTED_SIEVE_ASSERT_REPORT_CLOEXEC",
        "test hook: a distributed-sieve worker exits with a failure unless its "
        "report descriptor has FD_CLOEXEC",
    ),
    Exemption(
        "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_V1",
        "not an environment variable: stdout record prefix of the SIQS "
        "shadow-proof RSS holdout probe",
    ),
    Exemption(
        "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_ERROR_V1",
        "not an environment variable: error record prefix of the SIQS "
        "shadow-proof RSS holdout probe",
    ),
    Exemption(
        "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_V3",
        "not an environment variable: schema prefix of a joined SIQS "
        "shadow-proof RSS holdout transcript",
    ),
    Exemption(
        "GNFS_SIQS_SHADOW_PROOF_RSS_POLICY_V3",
        "not an environment variable: schema prefix of a SIQS shadow-proof "
        "RSS policy record",
    ),
    Exemption(
        "GNFS_CLI_PATH",
        "path override: the macOS workbench resolver tries this executable "
        "before its bundled gnfs; it does not tune the engine",
    ),
    Exemption(
        "GNFS_LP_BITS",
        "not a switch: no code reads it; it is a sample variable name in the "
        "workbench environment-isolation test (the real override is "
        "GNFS_OVERRIDE_LP_BITS)",
    ),
)


@dataclass(frozen=True, order=True)
class MissingName:
    name: str
    first_seen: str


@dataclass(frozen=True)
class ScanResult:
    missing: tuple[MissingName, ...]
    stale_exemptions: tuple[str, ...]
    documented: int
    exempt: int


def _line(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def _skip_pp_number(text: str, index: int) -> int:
    """Consume a preprocessing number, including its digit separators."""
    size = len(text)
    end = index + 1
    while end < size:
        char = text[end]
        if char in "eEpP" and end + 1 < size and text[end + 1] in "+-":
            end += 2
        elif char == "'" and end + 1 < size and (
            text[end + 1].isalnum() or text[end + 1] == "_"
        ):
            end += 2
        elif char.isalnum() or char in "._":
            end += 1
        else:
            break
    return end


def _skip_char_literal(text: str, index: int, relative: str) -> int:
    size = len(text)
    end = index + 1
    escaped = False
    while end < size:
        char = text[end]
        if char == "\n" and not escaped:
            break
        if char == "'" and not escaped:
            return end + 1
        escaped = char == "\\" and not escaped
        end += 1
    raise ContractError(f"{relative}:{_line(text, index)}: unterminated character literal")


def cxx_string_literals(text: str, relative: str) -> list[tuple[int, str]]:
    """Return (line, content) for every string literal in C, C++, or Objective-C source.

    The scan follows preprocessing-token boundaries, so comments, character
    literals, and numeric digit separators never open or close a string.
    """
    literals: list[tuple[int, str]] = []
    index = 0
    size = len(text)
    while index < size:
        char = text[index]
        if char.isspace():
            index += 1
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = size if end < 0 else end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise ContractError(
                    f"{relative}:{_line(text, index)}: unterminated block comment"
                )
            index = end + 2
            continue

        quote = index
        if char.isalpha() or char == "_":
            end = index
            while end < size and (text[end].isalnum() or text[end] == "_"):
                end += 1
            if text[index:end] not in CXX_STRING_PREFIXES or end >= size or text[end] != '"':
                index = end
                continue
            quote = end
        elif char == "'":
            index = _skip_char_literal(text, index, relative)
            continue
        elif char.isdigit() or (char == "." and index + 1 < size and text[index + 1].isdigit()):
            index = _skip_pp_number(text, index)
            continue
        elif char != '"':
            index += 1
            continue

        line = _line(text, index)
        if quote > index and text[quote - 1] == "R":
            opener = text.find("(", quote + 1, quote + 18)
            delimiter = text[quote + 1 : opener] if opener >= 0 else ""
            if opener < 0 or any(
                entry.isspace() or entry in "\\()" for entry in delimiter
            ):
                raise ContractError(f"{relative}:{line}: malformed raw string literal")
            terminator = ")" + delimiter + '"'
            end = text.find(terminator, opener + 1)
            if end < 0:
                raise ContractError(f"{relative}:{line}: unterminated raw string literal")
            literals.append((line, text[opener + 1 : end]))
            index = end + len(terminator)
            continue

        end = quote + 1
        escaped = False
        closed = False
        while end < size:
            char = text[end]
            if char == "\n" and not escaped:
                break
            if char == '"' and not escaped:
                closed = True
                break
            escaped = char == "\\" and not escaped
            end += 1
        if not closed:
            raise ContractError(f"{relative}:{line}: unterminated string literal")
        literals.append((line, text[quote + 1 : end]))
        index = end + 1
    return literals


def _skip_swift_block_comment(text: str, index: int, relative: str) -> int:
    size = len(text)
    depth = 1
    cursor = index + 2
    while cursor < size and depth:
        if text.startswith("/*", cursor):
            depth += 1
            cursor += 2
        elif text.startswith("*/", cursor):
            depth -= 1
            cursor += 2
        else:
            cursor += 1
    if depth:
        raise ContractError(f"{relative}:{_line(text, index)}: unterminated block comment")
    return cursor


def _read_swift_string(
    text: str, index: int, hashes: int, relative: str, literals: list[tuple[int, str]]
) -> int:
    """Consume a Swift string that starts at ``index`` and append its text segments."""
    size = len(text)
    quote = index + hashes
    line = _line(text, index)
    escape = "\\" + "#" * hashes
    if text.startswith('"""', quote):
        terminator = '"""' + "#" * hashes
        cursor = quote + 3
    else:
        terminator = '"' + "#" * hashes
        cursor = quote + 1
    start = cursor
    while cursor < size:
        if text.startswith(terminator, cursor):
            literals.append((line, text[start:cursor]))
            return cursor + len(terminator)
        if text.startswith(escape + "(", cursor):
            literals.append((line, text[start:cursor]))
            cursor = _skip_swift_interpolation(
                text, cursor + len(escape) + 1, relative, literals
            )
            start = cursor
            continue
        if text.startswith(escape, cursor):
            cursor += len(escape) + 1
            continue
        if terminator == '"' and text[cursor] == "\n":
            break
        cursor += 1
    raise ContractError(f"{relative}:{line}: unterminated Swift string literal")


def _skip_swift_interpolation(
    text: str, index: int, relative: str, literals: list[tuple[int, str]]
) -> int:
    """Consume a balanced ``\\(...)`` expression, collecting nested string literals."""
    size = len(text)
    depth = 1
    cursor = index
    while cursor < size:
        if text.startswith("//", cursor):
            end = text.find("\n", cursor)
            cursor = size if end < 0 else end
            continue
        if text.startswith("/*", cursor):
            cursor = _skip_swift_block_comment(text, cursor, relative)
            continue
        char = text[cursor]
        if char == "#":
            hashes = 0
            while cursor + hashes < size and text[cursor + hashes] == "#":
                hashes += 1
            if text.startswith('"', cursor + hashes):
                cursor = _read_swift_string(text, cursor, hashes, relative, literals)
                continue
            cursor += hashes
            continue
        if char == '"':
            cursor = _read_swift_string(text, cursor, 0, relative, literals)
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return cursor + 1
        cursor += 1
    raise ContractError(f"{relative}:{_line(text, index)}: unterminated Swift interpolation")


def swift_string_literals(text: str, relative: str) -> list[tuple[int, str]]:
    """Return (line, content) for the text segments of every Swift string literal."""
    literals: list[tuple[int, str]] = []
    index = 0
    size = len(text)
    while index < size:
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = size if end < 0 else end
            continue
        if text.startswith("/*", index):
            index = _skip_swift_block_comment(text, index, relative)
            continue
        if text[index] == "#":
            hashes = 0
            while index + hashes < size and text[index + hashes] == "#":
                hashes += 1
            if text.startswith('"', index + hashes):
                index = _read_swift_string(text, index, hashes, relative, literals)
                continue
            index += hashes
            continue
        if text[index] == '"':
            index = _read_swift_string(text, index, 0, relative, literals)
            continue
        index += 1
    return literals


def iter_source_files(root: Path) -> list[Path]:
    files: list[Path] = []

    def fail_walk(error: OSError) -> None:
        raise ContractError(f"cannot traverse source tree: {error}")

    for relative_root in SOURCE_ROOTS:
        source_root = root / relative_root
        if not source_root.is_dir():
            raise ContractError(f"{relative_root}: source root is missing")
        for directory, directory_names, file_names in os.walk(
            source_root, followlinks=False, onerror=fail_walk
        ):
            current = Path(directory)
            for name in directory_names:
                candidate = current / name
                if candidate.is_symlink():
                    raise ContractError(
                        f"{candidate.relative_to(root).as_posix()}: directory symlinks "
                        "are not scanned"
                    )
            for name in file_names:
                candidate = current / name
                relative = candidate.relative_to(root).as_posix()
                if candidate.suffix.lower() not in CXX_SUFFIXES | SWIFT_SUFFIXES:
                    continue
                if candidate.is_symlink():
                    raise ContractError(f"{relative}: source symlinks are not scanned")
                if not candidate.is_file():
                    raise ContractError(f"{relative}: unsupported non-regular source entry")
                files.append(candidate)
    return sorted(files)


def iter_doc_files(root: Path) -> list[Path]:
    doc_root = root / DOC_ROOT
    if not doc_root.is_dir():
        raise ContractError(f"{DOC_ROOT}: flag documentation root is missing")
    files = sorted(path for path in doc_root.rglob("*.md") if path.is_file())
    if not files:
        raise ContractError(f"{DOC_ROOT}: no flag documentation pages")
    return files


def _read(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ContractError(f"{relative}: cannot read UTF-8 source: {error}") from error
    if "\0" in text:
        raise ContractError(f"{relative}: NUL byte in scanned file")
    return text


def exemption_table(exemptions: tuple[Exemption, ...]) -> dict[str, str]:
    table: dict[str, str] = {}
    for exemption in exemptions:
        if not NAME_RE.fullmatch(exemption.name):
            raise ContractError(f"invalid exemption name {exemption.name!r}")
        if not exemption.reason.strip():
            raise ContractError(f"exemption {exemption.name} needs a reason")
        if exemption.name in table:
            raise ContractError(f"duplicate exemption {exemption.name}")
        table[exemption.name] = exemption.reason
    return table


def scan(root: Path, exemptions: tuple[Exemption, ...] = EXEMPTIONS) -> ScanResult:
    table = exemption_table(exemptions)
    first_seen: dict[str, str] = {}
    for path in iter_source_files(root):
        relative = path.relative_to(root).as_posix()
        text = _read(path, root)
        if path.suffix in SWIFT_SUFFIXES:
            literals = swift_string_literals(text, relative)
        else:
            literals = cxx_string_literals(text, relative)
        for line, literal in literals:
            for match in NAME_RE.finditer(literal):
                first_seen.setdefault(match.group(0), f"{relative}:{line}")

    doc_names: set[str] = set()
    for path in iter_doc_files(root):
        doc_names.update(match.group(0) for match in NAME_RE.finditer(_read(path, root)))

    missing = tuple(
        MissingName(name, first_seen[name])
        for name in sorted(first_seen)
        if name not in table and name not in doc_names
    )
    stale = tuple(sorted(name for name in table if name not in first_seen))
    return ScanResult(
        missing=missing,
        stale_exemptions=stale,
        documented=sum(1 for name in first_seen if name not in table),
        exempt=sum(1 for name in first_seen if name in table),
    )


def _expect_contract_error(label: str, action) -> None:
    try:
        action()
    except ContractError:
        return
    raise ContractError(f"self-test did not fail closed on {label}")


def _run_cli(root: Path) -> tuple[int, str]:
    """Run this script the way CI does and return its exit status and output."""
    completed = subprocess.run(
        [sys.executable, os.fspath(Path(__file__).resolve()), "--root", os.fspath(root)],
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.returncode, completed.stdout + completed.stderr


def run_self_test() -> None:
    """Exercise both tokenizers, the exact-token rule, exemptions, and fail-closed paths."""
    local = (Exemption("GNFS_HOOK_ATTEMPT_", "test hook"),)
    with tempfile.TemporaryDirectory(prefix="gnfs-env-flag-docs-") as temp_directory:
        root = Path(temp_directory)
        (root / "include" / "gnfs").mkdir(parents=True)
        (root / "src").mkdir()
        (root / "apps").mkdir()
        (root / DOC_ROOT).mkdir(parents=True)
        header = root / "include" / "gnfs" / "sample.hpp"
        exempt_header = root / "include" / "gnfs" / "exempt.hpp"
        source = root / "src" / "sample.cpp"
        broken = root / "src" / "broken.cpp"
        swift = root / "apps" / "Sample.swift"
        shell = root / "apps" / "legacy.sh"
        index_page = root / DOC_ROOT / "README.md"
        module_page = root / DOC_ROOT / "sample.md"

        header.write_text(
            '// "GNFS_COMMENT_ONLY" is mentioned in a comment\n'
            'const char* documented = "GNFS_DOCUMENTED";\n'
            'const char* undocumented = "GNFS_UNDOCUMENTED";\n'
            'const char* hook = ("GNFS_HOOK_ATTEMPT_" + std::to_string(1)).c_str();\n'
            'const char* embedded = "prefix_GNFS_EMBEDDED";\n'
            'const char* lowered = "GNFS_LOWER_case";\n'
            "auto separator = 1'000'000'000ULL;\n"
            "auto mask = 0xFFFF'FFFFULL;\n"
            "auto quote = '\"';\n"
            'const char* raw = R"x(GNFS_IN_RAW with " inside)x";\n'
            'const char8_t* prefixed = u8"GNFS_PREFIXED";\n'
            '/* "GNFS_BLOCK_COMMENT" */\n'
            'const char* prefix = "GNFS_PREFIX";\n',
            encoding="utf-8",
        )
        source.write_text('const char* other = "GNFS_DOCUMENTED";\n', encoding="utf-8")
        swift.write_text(
            'let raw = #"GNFS_SWIFT_RAW with " inside"#\n'
            'let nested = "outer \\(f("GNFS_SWIFT_NESTED")) tail"\n'
            'let multi = """\n'
            'GNFS_SWIFT_MULTI \\(value)\n'
            '"""\n'
            '// "GNFS_SWIFT_COMMENT"\n'
            '/* "GNFS_SWIFT_BLOCK" */\n'
            'let ignored = "prefix_GNFS_SWIFT_EMBEDDED"\n',
            encoding="utf-8",
        )
        # Files without a source suffix are never scanned, quoted or binary.
        shell.write_bytes(b'export GNFS_SHELL_IGNORED="GNFS_SHELL_QUOTED"\n')
        (root / "apps" / "icon.png").write_bytes(b"\x89PNG\r\n\x1a\n\xff\xfe")
        index_page.write_text("See GNFS_DOCUMENTED and GNFS_PREFIX_LONG.\n", encoding="utf-8")
        documented = (
            "`GNFS_SWIFT_RAW` `GNFS_SWIFT_NESTED` `GNFS_SWIFT_MULTI` `GNFS_IN_RAW`"
            " `GNFS_PREFIXED`\n"
        )
        module_page.write_text(documented, encoding="utf-8")

        # A longer documented name never covers a shorter code name.
        result = scan(root, local)
        observed = {entry.name: entry.first_seen for entry in result.missing}
        expected = {
            "GNFS_UNDOCUMENTED": "include/gnfs/sample.hpp:3",
            "GNFS_PREFIX": "include/gnfs/sample.hpp:13",
        }
        if observed != expected:
            raise ContractError(f"self-test collected unexpected names: {observed!r}")
        if result.stale_exemptions or result.exempt != 1 or result.documented != 8:
            raise ContractError("self-test miscounted documented or exempt names")

        # Comments, embedded identifiers, lower-case continuations, and
        # non-source files stay invisible even when nothing documents them.
        module_page.write_text(documented + "`GNFS_UNDOCUMENTED` `GNFS_PREFIX`\n", encoding="utf-8")
        clean = scan(root, local)
        if clean.missing or clean.stale_exemptions:
            raise ContractError("self-test rejected documented names")

        # The command line must name every gap, pass only a clean tree, and
        # reject a production exemption that no longer appears in the sources.
        exempt_lines = [
            f'const char* e{number} = "{entry.name}";\n'
            for number, entry in enumerate(EXEMPTIONS)
        ]
        exempt_header.write_text("".join(exempt_lines), encoding="utf-8")
        status, output = _run_cli(root)
        if status != 1 or "GNFS_HOOK_ATTEMPT_" not in output:
            raise ContractError("self-test accepted an undocumented name on the command line")
        module_page.write_text(
            module_page.read_text(encoding="utf-8") + "`GNFS_HOOK_ATTEMPT_`\n", encoding="utf-8"
        )
        status, output = _run_cli(root)
        if status != 0 or "[PASS]" not in output:
            raise ContractError(f"self-test rejected a clean tree: {output.strip()}")
        exempt_header.write_text("".join(exempt_lines[1:]), encoding="utf-8")
        status, output = _run_cli(root)
        if status != 1 or (
            f"exemption no longer present in source: {EXEMPTIONS[0].name}" not in output
        ):
            raise ContractError("self-test accepted a stale exemption on the command line")
        exempt_header.unlink()

        # An exemption whose name left the sources is a failure, not a free pass.
        header.write_text('const char* documented = "GNFS_DOCUMENTED";\n', encoding="utf-8")
        stale = scan(root, local)
        if stale.stale_exemptions != ("GNFS_HOOK_ATTEMPT_",):
            raise ContractError("self-test accepted a stale exemption")

        # Every exemption needs a valid, distinct name and a non-blank reason.
        if len(exemption_table(EXEMPTIONS)) != len(EXEMPTIONS):
            raise ContractError("self-test accepted duplicate exemptions")
        _expect_contract_error(
            "a lower-case exemption name",
            lambda: exemption_table((Exemption("gnfs_lower", "reason"),)),
        )
        _expect_contract_error(
            "a blank exemption reason",
            lambda: exemption_table((Exemption("GNFS_X", "  "),)),
        )
        _expect_contract_error(
            "a duplicate exemption",
            lambda: exemption_table((Exemption("GNFS_X", "a"), Exemption("GNFS_X", "b"))),
        )

        # Malformed sources, a missing root, and an empty doc tree fail closed.
        broken.write_text('const char* text = "unterminated;\n', encoding="utf-8")
        _expect_contract_error("an unterminated string", lambda: scan(root, local))
        broken.write_text("/* unterminated\n", encoding="utf-8")
        _expect_contract_error("an unterminated comment", lambda: scan(root, local))
        broken.unlink()
        swift.write_text('let text = "unterminated\n', encoding="utf-8")
        _expect_contract_error("an unterminated Swift string", lambda: scan(root, local))
        swift.unlink()
        (root / "src").rename(root / "moved_src")
        _expect_contract_error("a missing source root", lambda: scan(root, local))
        (root / "moved_src").rename(root / "src")
        module_page.unlink()
        index_page.unlink()
        _expect_contract_error("a doc root without pages", lambda: scan(root, local))


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
                print("[PASS] env flag documentation checker self-test")
            return 0

        root = (args.root or Path(__file__).resolve().parents[2]).resolve()
        result = scan(root)
    except ContractError as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    failed = False
    if result.missing:
        failed = True
        print(
            "[FAIL] undocumented GNFS_* names; document each one on its module page under "
            f"{DOC_ROOT}/ or add a reasoned exemption:",
            file=sys.stderr,
        )
        for entry in result.missing:
            print(f"[FAIL]   {entry.name} (first seen at {entry.first_seen})", file=sys.stderr)
    for name in result.stale_exemptions:
        failed = True
        print(f"[FAIL] exemption no longer present in source: {name}", file=sys.stderr)
    if failed:
        return 1

    if not args.quiet:
        print(
            f"[PASS] {result.documented} documented and {result.exempt} exempt GNFS_* names "
            f"found under {', '.join(SOURCE_ROOTS)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
