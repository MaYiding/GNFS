#!/usr/bin/env python3
"""Validate the closed GNFS environment inventory for distributed-sieve work identity."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


SOURCE_ROOTS = (
    "include/gnfs/sieve",
    "include/gnfs/cofactor",
    "src/sieve",
    "src/cofactor",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp"}

SEMANTIC_FLAGS = (
    "GNFS_LATTICE_LLL",
    "GNFS_LATTICE_SKEW",
    "GNFS_ADAPTIVE_LATTICE",
    "GNFS_ADAPTIVE_LATTICE_THRESHOLD",
    "GNFS_ADAPTIVE_LATTICE_MAX_RETRIES",
    "GNFS_ADAPTIVE_LATTICE_SEED",
    "GNFS_SURVIVAL_FILTER",
    "GNFS_SURVIVAL_THRESHOLD",
    "GNFS_COFACTOR_BRENT",
    "GNFS_ECM_BRENT_SUYAMA",
    "GNFS_ECM_BS_DEGREE",
    "GNFS_ECM_SIGMA_POOL_SIZE",
    "GNFS_ECM_CURVE_POOL",
)

CONSERVATIVE_FLAGS = (
    "GNFS_ECM_BATCH_INV",
    "GNFS_COFACTOR_BATCH_SIZE",
    "GNFS_BRENT_POLLARD_RHO_THREADS",
    "GNFS_ECM_B1_CACHE_SIZE",
    "GNFS_ECM_STAGE1_PARALLEL_THREADS",
    "GNFS_ECM_STAGE2_PARALLEL",
    "GNFS_COFACTOR_RESULT_CACHE_SIZE",
    "GNFS_TRIAL_DIV_SIMD",
    "GNFS_LATTICE_BASIS_PARALLEL_THREADS",
    "GNFS_LATTICE_COORDS_SIMD",
    "GNFS_SIEVE_APPLY_TILE_THREADS",
    "GNFS_BUCKET_PREFETCH",
    "GNFS_SIEVE_ECORE_THREADS",
    "GNFS_SIEVE_NO_TINY_SIMD",
    "GNFS_SIEVE_NORM_TILE_BITS",
    "GNFS_SIEVE_REGION_TILE_BITS",
    "GNFS_SIEVE_SATURATED_SUB_SIMD",
    "GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD",
)

DIAGNOSTIC_FLAGS = ("GNFS_COFACTOR_TIMING_ENABLE",)

DISTRIBUTED_FLAGS = (
    "GNFS_DISTRIBUTED_SIEVE_WORKERS",
    "GNFS_DISTRIBUTED_SIEVE_BASE_PATH",
    "GNFS_DISTRIBUTED_SIEVE_SQ_PER_WORKER",
)

CATEGORY_FLAGS = {
    "semantic": SEMANTIC_FLAGS,
    "conservative": CONSERVATIVE_FLAGS,
    "diagnostic": DIAGNOSTIC_FLAGS,
    "distributed": DISTRIBUTED_FLAGS,
}

# These implementation units are the environment-free side of the planned
# freeze boundary. Legacy parsing remains outside them.
DURABLE_ENVIRONMENT_FREE_FILES = {
    "include/gnfs/sieve/distributed_sieve_protocol.hpp",
    "include/gnfs/sieve/distributed_sieve_resume.hpp",
    "src/sieve/distributed_sieve_protocol.cpp",
    "src/sieve/distributed_sieve_resume.cpp",
}


@dataclass(frozen=True)
class LegacyDynamicRead:
    relative: str
    normalized_argument: str
    display_name: str


LEGACY_TEST_ONLY_READS = (
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_<chunk_id>",
    ),
)

LITERAL_ARGUMENT = re.compile(r'\s*"(GNFS_[A-Z0-9_]+)"\s*\Z')


@dataclass(frozen=True, order=True)
class GetenvCall:
    line: int
    argument: str


@dataclass(frozen=True, order=True)
class ClassifiedRead:
    category: str
    name: str
    relative: str
    line: int


def _skip_quoted(text: str, start: int, quote: str) -> int:
    cursor = start + 1
    while cursor < len(text):
        if text[cursor] == "\\":
            cursor += 2
            continue
        cursor += 1
        if text[cursor - 1] == quote:
            break
    return cursor


def _skip_raw_string(text: str, start: int) -> int | None:
    if start + 1 >= len(text) or text[start] != "R" or text[start + 1] != '"':
        return None
    delimiter_end = text.find("(", start + 2, min(len(text), start + 19))
    if delimiter_end < 0:
        return None
    delimiter = text[start + 2 : delimiter_end]
    if any(char.isspace() or char in "\\()" for char in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    end = text.find(terminator, delimiter_end + 1)
    return len(text) if end < 0 else end + len(terminator)


def _skip_non_code(text: str, cursor: int) -> int | None:
    raw_end = _skip_raw_string(text, cursor)
    if raw_end is not None:
        return raw_end
    if text[cursor] in {'"', "'"}:
        return _skip_quoted(text, cursor, text[cursor])
    if text.startswith("//", cursor):
        newline = text.find("\n", cursor + 2)
        return len(text) if newline < 0 else newline
    if text.startswith("/*", cursor):
        end = text.find("*/", cursor + 2)
        return len(text) if end < 0 else end + 2
    return None


def _matching_parenthesis(text: str, opening: int) -> int | None:
    depth = 1
    cursor = opening + 1
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text[cursor] == "(":
            depth += 1
        elif text[cursor] == ")":
            depth -= 1
            if depth == 0:
                return cursor
        cursor += 1
    return None


def _skip_call_trivia(text: str, cursor: int) -> int:
    while cursor < len(text):
        if text[cursor].isspace():
            cursor += 1
            continue
        if text.startswith("//", cursor) or text.startswith("/*", cursor):
            skipped = _skip_non_code(text, cursor)
            if skipped is None:
                return cursor
            cursor = skipped
            continue
        return cursor
    return cursor


def find_getenv_calls(text: str) -> tuple[list[GetenvCall], list[tuple[int, str]]]:
    calls: list[GetenvCall] = []
    errors: list[tuple[int, str]] = []
    cursor = 0
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text.startswith("getenv", cursor):
            before = text[cursor - 1] if cursor > 0 else ""
            after_index = cursor + len("getenv")
            after = text[after_index] if after_index < len(text) else ""
            if (before.isalnum() or before == "_") or (after.isalnum() or after == "_"):
                cursor += 1
                continue
            opening = _skip_call_trivia(text, after_index)
            if opening >= len(text) or text[opening] != "(":
                cursor += len("getenv")
                continue
            line = text.count("\n", 0, cursor) + 1
            closing = _matching_parenthesis(text, opening)
            if closing is None:
                errors.append((line, "unterminated getenv call"))
                break
            calls.append(GetenvCall(line=line, argument=text[opening + 1 : closing]))
            cursor = closing + 1
            continue
        cursor += 1
    return calls, errors


def normalize_dynamic_argument(argument: str) -> str:
    return re.sub(r"\s+", "", argument)


def build_flag_categories() -> tuple[dict[str, str], list[str]]:
    result: dict[str, str] = {}
    errors: list[str] = []
    for category, flags in CATEGORY_FLAGS.items():
        for flag in flags:
            previous = result.get(flag)
            if previous is not None:
                errors.append(f"{flag} is classified as both {previous} and {category}")
            result[flag] = category
    return result, errors


class Checks:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.errors: list[str] = []
        self.reads: list[ClassifiedRead] = []
        self.legacy_counts = {entry: 0 for entry in LEGACY_TEST_ONLY_READS}

    def fail(self, relative: str, line: int, message: str) -> None:
        self.errors.append(f"{relative}:{line}: {message}")

    def source_files(self) -> list[tuple[str, Path]]:
        files: list[tuple[str, Path]] = []
        for relative_root in SOURCE_ROOTS:
            directory = self.root / relative_root
            if not directory.is_dir():
                self.fail(relative_root, 1, "source inventory directory is missing")
                continue
            for path in directory.rglob("*"):
                if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                    relative = path.relative_to(self.root).as_posix()
                    files.append((relative, path))
        return sorted(files)

    def classify(self, relative: str, call: GetenvCall, categories: dict[str, str]) -> None:
        if relative in DURABLE_ENVIRONMENT_FREE_FILES:
            self.fail(
                relative,
                call.line,
                "durable protocol/resume implementation must not read process environment",
            )
            return

        literal = LITERAL_ARGUMENT.fullmatch(call.argument)
        if literal is not None:
            flag = literal.group(1)
            category = categories.get(flag)
            if category is None:
                self.fail(relative, call.line, f"unclassified GNFS environment read {flag}")
                return
            self.reads.append(
                ClassifiedRead(category=category, name=flag, relative=relative, line=call.line)
            )
            return

        normalized = normalize_dynamic_argument(call.argument)
        matches = [
            entry
            for entry in LEGACY_TEST_ONLY_READS
            if entry.relative == relative and entry.normalized_argument == normalized
        ]
        if len(matches) == 1:
            entry = matches[0]
            self.legacy_counts[entry] += 1
            self.reads.append(
                ClassifiedRead(
                    category="legacy-test-only",
                    name=entry.display_name,
                    relative=relative,
                    line=call.line,
                )
            )
            return

        rendered = " ".join(call.argument.split())
        if len(rendered) > 160:
            rendered = rendered[:157] + "..."
        self.fail(
            relative,
            call.line,
            f"dynamic or non-GNFS getenv read is not in the exact legacy-test-only allowlist: "
            f"{rendered!r}",
        )

    def run(self) -> list[str]:
        categories, category_errors = build_flag_categories()
        for error in category_errors:
            self.fail("scripts/check_distributed_sieve_policy.py", 1, error)

        for relative, path in self.source_files():
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                self.fail(relative, 1, f"cannot read source: {exc}")
                continue
            calls, parse_errors = find_getenv_calls(text)
            for line, error in parse_errors:
                self.fail(relative, line, error)
            for call in calls:
                self.classify(relative, call, categories)

        for entry, count in self.legacy_counts.items():
            if count != 1:
                self.fail(
                    entry.relative,
                    1,
                    f"legacy-test-only allowlist expects exactly one "
                    f"{entry.display_name} read, found {count}",
                )

        observed_flags = {
            read.name for read in self.reads if read.category != "legacy-test-only"
        }
        for flag, category in categories.items():
            if flag not in observed_flags:
                self.fail(
                    "scripts/check_distributed_sieve_policy.py",
                    1,
                    f"classified {category} setting {flag} has no source getenv read",
                )

        self.errors.sort()
        self.reads.sort()
        return self.errors


def run_self_test() -> list[str]:
    errors: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    snippet = r'''
// getenv("GNFS_NOT_A_REAL_READ")
const char* text = "getenv(\"GNFS_NOT_A_REAL_READ\")";
const char* semantic = std::getenv("GNFS_LATTICE_LLL");
const char* conservative = std::getenv /* inventory trivia */ ("GNFS_ECM_BATCH_INV");
const char* injected = std::getenv(
    ("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_" + std::to_string(chunk_id)).c_str());
'''
    calls, parse_errors = find_getenv_calls(snippet)
    expect(not parse_errors, f"unexpected parser errors: {parse_errors}")
    expect(len(calls) == 3, f"expected three real getenv calls, found {len(calls)}")
    if len(calls) == 3:
        literal = LITERAL_ARGUMENT.fullmatch(calls[0].argument)
        expect(
            literal is not None and literal.group(1) == "GNFS_LATTICE_LLL",
            "literal GNFS flag extraction failed",
        )
        conservative = LITERAL_ARGUMENT.fullmatch(calls[1].argument)
        expect(
            conservative is not None and conservative.group(1) == "GNFS_ECM_BATCH_INV",
            "comment-separated GNFS flag extraction failed",
        )
        expect(
            normalize_dynamic_argument(calls[2].argument)
            == LEGACY_TEST_ONLY_READS[0].normalized_argument,
            "exact dynamic test-injection normalization failed",
        )
        expect(
            calls[0].line == 4 and calls[1].line == 5 and calls[2].line == 6,
            "getenv line tracking failed",
        )

    categories, category_errors = build_flag_categories()
    expect(not category_errors, f"category table is not disjoint: {category_errors}")
    expect(
        categories.get("GNFS_COFACTOR_TIMING_ENABLE") == "diagnostic",
        "diagnostic classification failed",
    )
    expect(
        normalize_dynamic_argument(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"+std::to_string(other_chunk)).c_str()'
        )
        != LEGACY_TEST_ONLY_READS[0].normalized_argument,
        "legacy dynamic allowlist is not expression-exact",
    )

    unknown_checks = Checks(Path("."))
    unknown_checks.classify(
        "src/sieve/example.cpp",
        GetenvCall(line=17, argument='"GNFS_UNKNOWN_POLICY_FLAG"'),
        categories,
    )
    expect(
        unknown_checks.errors
        == [
            "src/sieve/example.cpp:17: "
            "unclassified GNFS environment read GNFS_UNKNOWN_POLICY_FLAG"
        ],
        "unknown GNFS flags do not fail with a source location",
    )

    legacy_checks = Checks(Path("."))
    legacy_entry = LEGACY_TEST_ONLY_READS[0]
    legacy_checks.classify(
        legacy_entry.relative,
        GetenvCall(line=23, argument=legacy_entry.normalized_argument),
        categories,
    )
    expect(
        legacy_checks.legacy_counts[legacy_entry] == 1
        and legacy_checks.reads
        == [
            ClassifiedRead(
                category="legacy-test-only",
                name=legacy_entry.display_name,
                relative=legacy_entry.relative,
                line=23,
            )
        ],
        "exact legacy dynamic read was not classified",
    )
    legacy_checks.classify(
        legacy_entry.relative,
        GetenvCall(
            line=29,
            argument=(
                '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"'
                "+std::to_string(other_chunk)).c_str()"
            ),
        ),
        categories,
    )
    expect(
        len(legacy_checks.errors) == 1
        and legacy_checks.errors[0].startswith(f"{legacy_entry.relative}:29:"),
        "altered legacy dynamic read does not fail with a source location",
    )

    durable_checks = Checks(Path("."))
    durable_relative = sorted(DURABLE_ENVIRONMENT_FREE_FILES)[0]
    durable_checks.classify(
        durable_relative,
        GetenvCall(line=31, argument='"GNFS_LATTICE_LLL"'),
        categories,
    )
    expect(
        durable_checks.errors
        == [
            f"{durable_relative}:31: "
            "durable protocol/resume implementation must not read process environment"
        ],
        "durable-path environment ban is not enforced",
    )
    return errors


def print_inventory(reads: list[ClassifiedRead]) -> None:
    for category in (*CATEGORY_FLAGS.keys(), "legacy-test-only"):
        category_reads = [read for read in reads if read.category == category]
        names = sorted({read.name for read in category_reads})
        print(f"[INFO] {category}: {len(names)} settings, {len(category_reads)} reads")
        for read in category_reads:
            print(f"  {read.name} -> {read.relative}:{read.line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quiet", action="store_true", help="print only failures")
    parser.add_argument("--root", type=Path, help="repository root (defaults to this script's parent)")
    parser.add_argument("--self-test", action="store_true", help="run parser/table self-tests first")
    args = parser.parse_args()

    if args.self_test:
        self_test_errors = run_self_test()
        if self_test_errors:
            for error in self_test_errors:
                print(
                    f"[FAIL] scripts/check_distributed_sieve_policy.py:1: self-test: {error}",
                    file=sys.stderr,
                )
            return 1
        if not args.quiet:
            print("[PASS] distributed-sieve policy checker self-test")

    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    checks = Checks(root)
    errors = checks.run()
    if errors:
        for error in errors:
            print(f"[FAIL] {error}", file=sys.stderr)
        return 1
    if not args.quiet:
        print_inventory(checks.reads)
        print("[PASS] distributed-sieve GNFS environment inventory is closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
