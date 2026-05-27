#!/usr/bin/env python3
"""Check local Markdown links in tracked documentation files."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


INLINE_LINK_RE = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)")
REF_LINK_RE = re.compile(r"^\s*\[[^\]]+\]:\s*(\S+)", re.MULTILINE)
SKIP_SCHEMES = {
    "http",
    "https",
    "mailto",
    "ftp",
    "file",
    "app",
}


def tracked_markdown_files(repo: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "*.md"],
        cwd=repo,
        text=True,
        check=True,
        stdout=subprocess.PIPE,
    )
    return [repo / line for line in result.stdout.splitlines() if line]


def split_target(raw: str) -> str:
    target = raw.strip()
    if target.startswith("<"):
        end = target.find(">")
        if end != -1:
            return target[1:end]
    return target.split()[0] if target else ""


def is_external(target: str) -> bool:
    parsed = urlparse(target)
    return parsed.scheme.lower() in SKIP_SCHEMES


def mask_code(text: str) -> str:
    """Mask fenced and inline code while preserving line numbers."""
    masked_lines: list[str] = []
    in_fence = False
    fence_marker = ""

    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if not in_fence:
                in_fence = True
                fence_marker = marker
            elif marker == fence_marker:
                in_fence = False
                fence_marker = ""
            masked_lines.append("\n" if line.endswith("\n") else "")
            continue

        if in_fence:
            masked_lines.append("\n" if line.endswith("\n") else "")
            continue

        masked_lines.append(re.sub(r"`[^`\n]*`", lambda m: " " * len(m.group(0)), line))

    return "".join(masked_lines)


def iter_links(text: str) -> list[tuple[int, str]]:
    links: list[tuple[int, str]] = []
    for regex in (INLINE_LINK_RE, REF_LINK_RE):
        for match in regex.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            target = split_target(match.group(1))
            if target:
                links.append((line, target))
    return links


def check_link(repo: Path, source: Path, line: int, target: str) -> str | None:
    if is_external(target) or target.startswith("#"):
        return None

    path_part, _, _fragment = target.partition("#")
    path_part = unquote(path_part)
    if not path_part:
        return None

    candidate = (source.parent / path_part).resolve()
    try:
        candidate.relative_to(repo)
    except ValueError:
        return f"{source.relative_to(repo)}:{line}: link escapes repository: {target}"

    if not candidate.exists():
        return f"{source.relative_to(repo)}:{line}: missing local link target: {target}"

    return None


def main() -> int:
    repo = Path.cwd().resolve()
    failures: list[str] = []

    for path in tracked_markdown_files(repo):
        text = mask_code(path.read_text(encoding="utf-8"))
        for line, target in iter_links(text):
            failure = check_link(repo, path, line, target)
            if failure:
                failures.append(failure)

    if failures:
        print("Broken local Markdown links:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Local Markdown links are valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
