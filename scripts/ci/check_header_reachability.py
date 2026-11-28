#!/usr/bin/env python3
"""Pin the headers under include/gnfs that no production source reaches.

The closure starts at every C and C++ source outside ``tests/`` and ``bench/``
and at every file that a ``/FI`` option forces into a translation unit. It then
follows ``#include "..."`` and ``#include <gnfs/...>`` lines. A header under
``include/gnfs`` that the closure misses has to be listed in
``scripts/ci/production-unreachable-headers.txt`` together with the gate that
guards it, and a listed header has to stay unreachable. The check reads text
only and calls no compiler.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


PUBLIC_INCLUDE = "include"
PUBLIC_HEADER_ROOT = "include/gnfs"
MANIFEST = "scripts/ci/production-unreachable-headers.txt"
# Sources below these directories are never roots.
NON_PRODUCTION_DIRECTORIES = ("tests", "bench")
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".mm", ".c")
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tcc")
CMAKE_SUFFIXES = ("CMakeLists.txt", ".cmake")
# A quoted include is searched in the directory of the including file, then in
# every src/<module>/ directory that holds a file, then in include/.
PRIVATE_SOURCE_ROOT = "src"
# CMake variables that expand to the repository root in a /FI argument.
SOURCE_DIRECTORY_VARIABLES = (
    "${CMAKE_CURRENT_SOURCE_DIR}",
    "${CMAKE_SOURCE_DIR}",
    "${PROJECT_SOURCE_DIR}",
)

# The tokens that end plain code: a comment, a quote, or a raw string literal,
# which opens with R"delimiter( and may span lines.
_TOKEN_RE = re.compile(
    r'//[^\n]*|/\*|"|\'|(?<![A-Za-z0-9_])(?:u8|u|U|L)?R"([^ ()\\\t\v\f\r\n]*)\('
)
_INCLUDE_LINE_RE = re.compile(r"[ \t]*#[ \t]*include[ \t]*(.*?)[ \t]*")
_INCLUDE_TARGET_RE = re.compile(r'^(?:"([^"\n]+)"|<([^>\n]+)>)$')
# /FI starts a token and names a forced include.
_FORCED_INCLUDE_RE = re.compile(r'(?<![A-Za-z0-9_./\\-])/FI[ \t]*(?:"([^"\n]+)"|([^\s")]+))')


class ReachabilityCheckError(RuntimeError):
    """The checker did not judge a self-test fixture the expected way."""


def strip_comments(text: str) -> str:
    """Blank C and C++ comments and raw string bodies, keeping every newline.

    An include directive counts by the line it starts, so a directive inside a
    comment must not count, and a comment marker or a quote inside a string
    literal must not hide a directive on a later line. Ordinary string and
    character literals stay as written: they hold the paths of quoted includes
    and cannot span lines.
    """
    out: list[str] = []
    position = 0
    while True:
        match = _TOKEN_RE.search(text, position)
        if match is None:
            out.append(text[position:])
            return "".join(out)
        start = match.start()
        out.append(text[position:start])
        token = match.group(0)
        if token.startswith("//"):
            end = len(text) if "\n" not in text[start:] else text.index("\n", start)
        elif token == "/*":
            close = text.find("*/", start + 2)
            end = len(text) if close == -1 else close + 2
        elif token in ('"', "'"):
            index = match.end()
            while index < len(text) and text[index] != token and text[index] != "\n":
                index += 2 if text[index] == "\\" else 1
            end = index + 1 if index < len(text) and text[index] == token else index
            out.append(text[start:end])
            position = end
            continue
        else:
            terminator = ")" + match.group(1) + '"'
            close = text.find(terminator, match.end())
            end = len(text) if close == -1 else close + len(terminator)
        out.append(re.sub(r"[^\n]", " ", text[start:end]))
        position = end


def strip_cmake_comments(text: str) -> str:
    """Blank the # comments of a CMake file and keep its quoted arguments."""
    lines = []
    for line in text.split("\n"):
        quoted = False
        cut = len(line)
        for index, char in enumerate(line):
            if char == '"':
                quoted = not quoted
            elif char == "#" and not quoted and (index == 0 or line[index - 1] in " \t"):
                cut = index
                break
        lines.append(line[:cut])
    return "\n".join(lines)


def read_text(path: Path) -> str:
    """Read one text file, whatever byte order mark or undecodable byte it has."""
    return path.read_text(encoding="utf-8-sig", errors="replace")


def repository_files(repo: Path) -> list[str]:
    """Return the tracked files and the untracked files that Git does not ignore."""
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=repo,
        check=True,
        stdout=subprocess.PIPE,
    )
    names = result.stdout.decode("utf-8", "surrogateescape").split("\0")
    return sorted({name for name in names if name})


def production_sources(files: list[str]) -> list[str]:
    """Return the C and C++ sources outside the non-production directories."""
    return [
        name
        for name in files
        if name.endswith(SOURCE_SUFFIXES)
        and name.partition("/")[0] not in NON_PRODUCTION_DIRECTORIES
    ]


def private_source_directories(files: list[str]) -> list[str]:
    """Return the first-level directories below src/ that hold a file."""
    offset = len(PRIVATE_SOURCE_ROOT) + 1
    return sorted(
        {
            name.split("/")[1]
            for name in files
            if name.startswith(PRIVATE_SOURCE_ROOT + "/") and "/" in name[offset:]
        }
    )


def forced_includes(repo: Path, files: list[str]) -> tuple[list[str], list[str]]:
    """Return the files that /FI options force into every translation unit.

    The second element holds one warning for every forced include that names
    no file of the tree.
    """
    inventory = set(files)
    forced: list[str] = []
    warnings: list[str] = []
    for name in files:
        if not name.endswith(CMAKE_SUFFIXES):
            continue
        try:
            text = strip_cmake_comments(read_text(repo / name))
        except OSError as error:
            warnings.append(f"cannot read {name}: {error.strerror or error}")
            continue
        directory = posixpath.dirname(name)
        for number, line in enumerate(text.split("\n"), start=1):
            for match in _FORCED_INCLUDE_RE.finditer(line):
                path = (match.group(1) or match.group(2)).replace("\\", "/")
                base = directory
                for variable in SOURCE_DIRECTORY_VARIABLES:
                    if path.startswith(variable):
                        base = ""
                        path = path[len(variable) :]
                        break
                if "${" in path:
                    warnings.append(
                        f"cannot resolve forced include {path} from {name}:{number}"
                    )
                    continue
                candidate = posixpath.normpath(posixpath.join(base, path.lstrip("/")))
                if candidate in inventory:
                    forced.append(candidate)
                else:
                    warnings.append(
                        f"forced include {candidate} from {name}:{number} does not exist"
                    )
    return forced, warnings


def resolve_include(
    spec: str,
    angled: bool,
    includer: str,
    inventory: set[str],
    private_directories: list[str],
) -> str | None:
    """Return the tracked file that an include names, or ``None``.

    Only a tracked file counts, so a path that leaves the tree or differs in
    case from the tracked name resolves to nothing.
    """
    if angled:
        candidates = [f"{PUBLIC_INCLUDE}/{spec}"]
    else:
        candidates = [posixpath.join(posixpath.dirname(includer), spec)]
        candidates += [
            posixpath.join(PRIVATE_SOURCE_ROOT, module, spec) for module in private_directories
        ]
        candidates.append(posixpath.join(PUBLIC_INCLUDE, spec))
    for candidate in candidates:
        normalized = posixpath.normpath(candidate)
        if normalized in inventory:
            return normalized
    return None


def included_files(
    repo: Path,
    name: str,
    inventory: set[str],
    private_directories: list[str],
) -> tuple[list[str], list[tuple[int, str]]]:
    """Return the files that ``name`` includes and the lines that name none.

    Each unresolved entry is ``(line, directive)``. An angle include outside
    gnfs/ names a system header and is skipped without a report.
    """
    targets: list[str] = []
    unresolved: list[tuple[int, str]] = []
    for number, line in enumerate(strip_comments(read_text(repo / name)).split("\n"), start=1):
        match = _INCLUDE_LINE_RE.fullmatch(line)
        if match is None:
            continue
        target = _INCLUDE_TARGET_RE.match(match.group(1))
        if target is None:
            unresolved.append((number, f"#include {match.group(1)}"))
            continue
        spec = target.group(1) or target.group(2)
        angled = target.group(1) is None
        if angled and not spec.startswith("gnfs/"):
            continue
        resolved = resolve_include(spec, angled, name, inventory, private_directories)
        if resolved is None:
            unresolved.append((number, f"#include <{spec}>" if angled else f'#include "{spec}"'))
        elif resolved not in targets:
            targets.append(resolved)
    return targets, unresolved


def include_closure(
    repo: Path,
    roots: list[str],
    inventory: set[str],
    private_directories: list[str],
    warnings: list[str],
) -> set[str]:
    """Return every tracked file that the roots reach, and report what they miss.

    ``warnings`` receives one line per directive that names no tracked file and
    per root that cannot be read.
    """
    reachable: set[str] = set()
    unresolved: dict[str, list[tuple[str, int]]] = {}
    cache: dict[str, tuple[list[str], list[tuple[int, str]]]] = {}
    pending = list(roots)
    while pending:
        name = pending.pop()
        if name in reachable:
            continue
        if name not in inventory:
            continue
        reachable.add(name)
        if name not in cache:
            try:
                cache[name] = included_files(repo, name, inventory, private_directories)
            except OSError as error:
                warnings.append(f"cannot read {name}: {error.strerror or error}")
                cache[name] = ([], [])
        targets, missing = cache[name]
        for line, directive in missing:
            unresolved.setdefault(directive, []).append((name, line))
        pending.extend(targets)
    for directive, places in sorted(unresolved.items()):
        first_file, first_line = min(places)
        if len(places) > 1:
            directive += f" from {first_file}:{first_line} and {len(places) - 1} more"
        else:
            directive += f" from {first_file}:{first_line}"
        warnings.append(f"cannot resolve {directive}")
    return reachable


def parse_manifest(text: str) -> tuple[list[str], list[str], set[str]]:
    """Return the listed paths, one message for every malformed line, and the
    malformed paths themselves."""
    entries: list[str] = []
    errors: list[str] = []
    rejected: set[str] = set()
    for number, line in enumerate(text.split("\n"), start=1):
        path, separator, comment = line.partition("#")
        entry = path.strip()
        if not entry:
            continue
        where = f"{MANIFEST}:{number}"
        if not separator or not comment.strip():
            errors.append(f"{where}: {entry} needs a trailing comment that names its gate")
            rejected.add(entry)
        if posixpath.normpath(entry) != entry:
            errors.append(f"{where}: {entry} is not a normalized repository-relative path")
            rejected.add(entry)
        elif not entry.startswith(PUBLIC_HEADER_ROOT + "/"):
            errors.append(f"{where}: {entry} is not below {PUBLIC_HEADER_ROOT}/")
            rejected.add(entry)
        elif entry in entries:
            errors.append(f"{where}: {entry} is listed twice")
        entries.append(entry)
    return entries, errors, rejected


@dataclass
class Report:
    """What the checker found in one tree, every path relative to its root."""

    roots: list[str] = field(default_factory=list)
    reachable: set[str] = field(default_factory=set)
    headers: list[str] = field(default_factory=list)
    listed: list[str] = field(default_factory=list)
    unreachable: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def analyze(repo: Path, files: list[str], manifest_text: str) -> Report:
    """Judge one tree. ``files`` holds its repository-relative paths."""
    inventory = set(files)
    private_directories = private_source_directories(files)
    report = Report()
    forced, report.warnings = forced_includes(repo, files)
    roots = production_sources(files)
    roots += [name for name in forced if name not in roots]
    report.roots = sorted(roots)
    report.reachable = include_closure(
        repo, roots, inventory, private_directories, report.warnings
    )
    report.warnings.sort()
    report.headers = sorted(
        name
        for name in files
        if name.startswith(PUBLIC_HEADER_ROOT + "/") and name.endswith(HEADER_SUFFIXES)
    )
    report.unreachable = [name for name in report.headers if name not in report.reachable]
    report.listed, report.errors, rejected = parse_manifest(manifest_text)
    listed = set(report.listed)
    for header in report.unreachable:
        if header not in listed:
            report.errors.append(new_header_error(header))
    for entry in report.listed:
        if entry in rejected:
            continue
        if entry not in inventory:
            report.errors.append(stale_missing_error(entry))
        elif entry in report.reachable:
            report.errors.append(stale_reached_error(entry))
    return report


def new_header_error(header: str) -> str:
    return (
        f"new production-unreachable header: {header}; no source outside tests/ and bench/ "
        f"reaches it and {MANIFEST} does not list it. Wire it into production, or list it "
        "with the gate that guards it."
    )


def stale_reached_error(entry: str) -> str:
    return (
        f"stale manifest entry: {entry} is reached from production now; "
        f"remove it from {MANIFEST}."
    )


def stale_missing_error(entry: str) -> str:
    return (
        f"stale manifest entry: {entry} does not exist; remove it from {MANIFEST} "
        "or correct the path."
    )


def run_self_test() -> None:
    """Run the checker on a temporary tree and on a Git checkout of it.

    The tree covers angle and quoted includes, a nested include, a bare private
    name found through src/<module>/, a root of every source suffix, a forced
    include in each spelling, directives that only a comment or a string holds,
    and a manifest with one entry too many and one entry too few.
    """
    reached = (
        "include/gnfs/angle.hpp",
        "include/gnfs/from_c.hpp",
        "include/gnfs/from_cc.hpp",
        "include/gnfs/from_cxx.hpp",
        "include/gnfs/from_tight.hpp",
        "include/gnfs/nested.hpp",
        "include/gnfs/util/back_reference.hpp",
        "include/gnfs/util/msvc_compat.hpp",
        "include/gnfs/util/named.hpp",
        "include/gnfs/util/nested_forced.hpp",
        "include/gnfs/util/private_user.hpp",
        "include/gnfs/util/quoted_forced.hpp",
    )
    unreachable = (
        "include/gnfs/util/bench_only.hpp",
        "include/gnfs/util/in_block_comment.hpp",
        "include/gnfs/util/in_cmake_comment.hpp",
        "include/gnfs/util/in_line_comment.hpp",
        "include/gnfs/util/in_multiline_comment.hpp",
        "include/gnfs/util/in_raw_string.hpp",
        "include/gnfs/util/in_string.hpp",
        "include/gnfs/util/listed.hpp",
        "include/gnfs/util/test_only.hpp",
    )
    tree = {
        "CMakeLists.txt": [
            "cmake_minimum_required(VERSION 3.20)",
            "# A comment names no forced include: /FI include/gnfs/util/in_cmake_comment.hpp",
            "add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/include/gnfs/util/msvc_compat.hpp)",
            'add_compile_options("/FI${CMAKE_CURRENT_SOURCE_DIR}/include/gnfs/util/quoted_forced.hpp")',
            "add_compile_options(/FI${TOOLCHAIN_DIR}/forced.h)",
            "add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/include/gnfs/util/absent_forced.hpp)",
        ],
        "apps/CMakeLists.txt": ["add_compile_options(/FI ../include/gnfs/util/nested_forced.hpp)"],
        "src/plain.cpp": [
            "#include <gnfs/angle.hpp>",
            '#include "missing_local.hpp"',
            "#include <gnfs/util/missing_public.hpp>",
            "#include GNFS_CONFIG_HEADER",
            "#include <vector>",
            '#include "leaf_private.hpp"',
            '#include "../../outside.hpp"',
        ],
        "src/other.cc": ['#include "missing_local.hpp"', "#include <gnfs/from_cc.hpp>"],
        "src/util/leaf_private.hpp": ["#include <gnfs/util/private_user.hpp>"],
        "apps/tool.mm": ['#include "gnfs/util/named.hpp"', "#include <gnfs/util/Listed.hpp>"],
        "apps/native.cxx": ["#include  <gnfs/from_cxx.hpp>", "#   include <gnfs/from_tight.hpp>"],
        "apps/legacy.c": ["#include <gnfs/from_c.hpp>", "#include <gnfs/from_cxx.hpp>"],
        "tests/test_only.cpp": ["#include <gnfs/util/test_only.hpp>"],
        "bench/bench_only.cpp": [
            "#include <gnfs/util/test_only.hpp>",
            "#include <gnfs/util/bench_only.hpp>",
        ],
        "include/gnfs/readme.txt": ["Not a header, so the manifest never needs it."],
        "include/gnfs/angle.hpp": [
            "// #include <gnfs/util/in_line_comment.hpp>",
            "/* #include <gnfs/util/in_block_comment.hpp>",
            "   #include <gnfs/util/in_multiline_comment.hpp> */",
            'static const char* text = "// #include <gnfs/util/in_string.hpp>";',
            'static const char* raw = R"mark(',
            "#include <gnfs/util/in_raw_string.hpp>",
            ')mark";',
            '#include "nested.hpp"',
        ],
        "include/gnfs/nested.hpp": ['#include "../gnfs/util/back_reference.hpp"'],
    }
    # The other headers that the sources name do not exist.
    headers = reached + unreachable
    roots = sorted(
        [
            "apps/legacy.c",
            "apps/native.cxx",
            "apps/tool.mm",
            "include/gnfs/util/msvc_compat.hpp",
            "include/gnfs/util/nested_forced.hpp",
            "include/gnfs/util/quoted_forced.hpp",
            "src/other.cc",
            "src/plain.cpp",
        ]
    )
    warnings = sorted(
        [
            'cannot resolve #include "../../outside.hpp" from src/plain.cpp:7',
            'cannot resolve #include "missing_local.hpp" from src/other.cc:1 and 1 more',
            "cannot resolve #include <gnfs/util/Listed.hpp> from apps/tool.mm:2",
            "cannot resolve #include <gnfs/util/missing_public.hpp> from src/plain.cpp:3",
            "cannot resolve #include GNFS_CONFIG_HEADER from src/plain.cpp:4",
            "cannot resolve forced include ${TOOLCHAIN_DIR}/forced.h from CMakeLists.txt:5",
            "forced include include/gnfs/util/absent_forced.hpp from CMakeLists.txt:6 does not exist",
        ]
    )

    def write(root: Path, name: str, *lines: str) -> None:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def manifest(*entries: str) -> str:
        return "\n".join(
            ["# One repository-relative path per line, then the gate that guards it."]
            + [f"{entry}  # GNFS_TEST_GATE" for entry in entries]
        )

    def tree_files(root: Path) -> list[str]:
        return sorted(
            path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
        )

    def check(root: Path, text: str) -> Report:
        return analyze(root, tree_files(root), text)

    def expect(condition: bool, message: str, *found: object) -> None:
        if not condition:
            raise ReachabilityCheckError(f"self-test: {message}: {list(found)!r}")

    with tempfile.TemporaryDirectory(prefix="gnfs-header-reachability-") as temp_directory:
        root = Path(temp_directory).resolve()
        for name, lines in tree.items():
            write(root, name, *lines)
        for name in headers:
            if name not in tree:
                write(root, name, "#pragma once")
        baseline = manifest(*unreachable)
        write(root, MANIFEST, baseline)

        # Angle, quoted, nested, private, and forced includes reach what they
        # should; tests/ and bench/ stay outside the roots; comments and
        # strings hide their directives; a path that leaves the tree or
        # differs in case from the tracked name, and every unresolved include,
        # only warns.
        report = check(root, baseline)
        expect(report.errors == [], "the baseline tree must pass", report.errors)
        reached_headers = sorted(name for name in report.reachable if name.startswith("include/"))
        expect(reached_headers == list(reached), "wrong reached headers", reached_headers)
        expect(
            report.unreachable == sorted(unreachable),
            "wrong unreachable headers",
            report.unreachable,
        )
        expect(report.roots == roots, "wrong roots", report.roots)
        expect(report.warnings == warnings, "wrong warnings", report.warnings)

        # One entry too few: an unlisted header that nothing reaches fails.
        without_listed = [name for name in unreachable if not name.endswith("/listed.hpp")]
        report = check(root, manifest(*without_listed))
        expect(
            report.errors == [new_header_error("include/gnfs/util/listed.hpp")],
            "a missing entry must fail",
            report.errors,
        )

        # One entry too many: a reached header and an absent path both fail.
        extra = manifest(
            *unreachable, "include/gnfs/angle.hpp", "include/gnfs/util/never_written.hpp"
        )
        report = check(root, extra)
        expect(
            report.errors
            == [
                stale_reached_error("include/gnfs/angle.hpp"),
                stale_missing_error("include/gnfs/util/never_written.hpp"),
            ],
            "an extra entry must fail",
            report.errors,
        )

        # Without /FI nothing reaches the forced headers.
        write(root, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)")
        write(root, "apps/CMakeLists.txt", "# no forced include")
        forced = sorted(
            (
                "include/gnfs/util/msvc_compat.hpp",
                "include/gnfs/util/nested_forced.hpp",
                "include/gnfs/util/quoted_forced.hpp",
            )
        )
        report = check(root, baseline)
        expect(
            report.errors == [new_header_error(name) for name in forced],
            "removing /FI must fail",
            report.errors,
        )
        write(root, "CMakeLists.txt", *tree["CMakeLists.txt"])
        write(root, "apps/CMakeLists.txt", *tree["apps/CMakeLists.txt"])

        # A listed header that production starts to include is stale.
        write(root, "src/other.cc", *tree["src/other.cc"], "#include <gnfs/util/test_only.hpp>")
        report = check(root, baseline)
        expect(
            report.errors == [stale_reached_error("include/gnfs/util/test_only.hpp")],
            "a reached listed header must fail",
            report.errors,
        )
        write(root, "src/other.cc", *tree["src/other.cc"])

        # Every source suffix is a root, and losing one is visible.
        (root / "apps" / "legacy.c").unlink()
        report = check(root, baseline)
        expect(
            report.errors == [new_header_error("include/gnfs/from_c.hpp")],
            "losing the .c root must fail",
            report.errors,
        )
        write(root, "apps/legacy.c", *tree["apps/legacy.c"])

        # A malformed manifest line fails, wherever it sits.
        malformed = "\n".join(
            [
                "include/gnfs/util/listed.hpp",
                "include/gnfs/util/./listed.hpp  # not normalized",
                "include/gnfs/util/test_only.hpp  # first",
                "include/gnfs/util/test_only.hpp  # twice",
                "src/plain.cpp  # not a public header",
            ]
        )
        report = check(root, malformed)
        expect(
            report.errors[:4]
            == [
                f"{MANIFEST}:1: include/gnfs/util/listed.hpp needs a trailing comment that names its gate",
                f"{MANIFEST}:2: include/gnfs/util/./listed.hpp is not a normalized repository-relative path",
                f"{MANIFEST}:4: include/gnfs/util/test_only.hpp is listed twice",
                f"{MANIFEST}:5: src/plain.cpp is not below {PUBLIC_HEADER_ROOT}/",
            ]
            and report.errors[4:]
            == [
                new_header_error(name)
                for name in unreachable
                if "listed" not in name and "test_only" not in name
            ],
            "a malformed line must fail",
            report.errors,
        )

        # The command line reads a Git checkout: tracked files, untracked files
        # that Git does not ignore, and nothing below an ignored path.
        for arguments in (["git", "init", "-q"], ["git", "add", "-A"]):
            subprocess.run(
                arguments, cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )

        def run_checker() -> subprocess.CompletedProcess:
            return subprocess.run(
                [sys.executable, str(Path(__file__).resolve())],
                cwd=root,
                text=True,
                encoding="utf-8",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

        result = run_checker()
        reported = sorted(line[len("warning: ") :] for line in result.stderr.splitlines())
        expect(
            result.returncode == 0
            and result.stdout.startswith("Production-unreachable headers match ")
            and reported == warnings,
            "the checkout must pass",
            result.returncode,
            result.stdout,
            result.stderr,
        )

        write(root, MANIFEST, manifest(*without_listed))
        result = run_checker()
        expect(
            result.returncode == 1
            and new_header_error("include/gnfs/util/listed.hpp") in result.stdout,
            "a missing entry must exit 1",
            result.returncode,
            result.stdout,
            result.stderr,
        )

        write(root, ".gitignore", "ignored/")
        write(root, "include/gnfs/util/untracked.hpp", "#pragma once")
        write(root, "src/new_root.cpp", "#include <gnfs/util/untracked.hpp>")
        write(root, "include/gnfs/util/ignored_only.hpp", "#pragma once")
        write(root, "ignored/junk.cpp", "#include <gnfs/util/ignored_only.hpp>")
        write(
            root,
            MANIFEST,
            manifest(
                *unreachable,
                "include/gnfs/util/untracked.hpp",
                "include/gnfs/util/ignored_only.hpp",
            ),
        )
        result = run_checker()
        expect(
            result.returncode == 1
            and result.stdout.count("  - ") == 1
            and stale_reached_error("include/gnfs/util/untracked.hpp") in result.stdout,
            "only the untracked root must count",
            result.returncode,
            result.stdout,
            result.stderr,
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--self-test", action="store_true", help="run checker regression tests")
    args = parser.parse_args()

    try:
        if args.self_test:
            run_self_test()
            print("[PASS] Header reachability checker self-test")
            return 0

        repo = Path.cwd().resolve()
        report = analyze(repo, repository_files(repo), read_text(repo / MANIFEST))
    except (ReachabilityCheckError, OSError, subprocess.CalledProcessError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    for warning in report.warnings:
        print(f"warning: {warning}", file=sys.stderr)

    if report.errors:
        print(f"Headers under {PUBLIC_HEADER_ROOT} do not match {MANIFEST}:")
        for error in report.errors:
            print(f"  - {error}")
        return 1

    listed = len(report.listed)
    print(
        f"Production-unreachable headers match {MANIFEST} "
        f"({listed} listed, {len(report.headers) - listed} of {len(report.headers)} headers "
        f"under {PUBLIC_HEADER_ROOT} reached from production)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
