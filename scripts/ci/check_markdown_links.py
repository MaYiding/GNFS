#!/usr/bin/env python3
"""Check local Markdown links in tracked documentation files, anchors included."""

from __future__ import annotations

import argparse
import html
import re
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path
from urllib.parse import unquote, urlparse


INLINE_LINK_RE = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)")
REF_LINK_RE = re.compile(r"^[ \t]*\[[^\]]+\]:[ \t]*(\S+)", re.MULTILINE)
SKIP_SCHEMES = {
    "http",
    "https",
    "mailto",
    "ftp",
    "file",
    "app",
}
# Block-quote markers and list bullets that may precede a heading.
CONTAINER_PREFIX_RE = re.compile(r"^ {0,3}(?:>[ \t]?|[-*+][ \t]+|\d{1,9}[.)][ \t])")
ATX_HEADING_RE = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*))?$")
ATX_CLOSING_RE = re.compile(r"[ \t]+#+$")
SETEXT_UNDERLINE_RE = re.compile(r"^ {0,3}(=+|-+)[ \t]*$")
# Lines that cannot be the text of a Setext heading: indented code, list
# items, table rows, and HTML blocks (a tag alone on the line, or a comment,
# processing instruction, or declaration opener).
NON_PARAGRAPH_RE = re.compile(
    r"^ {4}|^[-*+][ \t]|^\d{1,9}[.)][ \t]|^\||^<!--|^<[!?]"
    r"|^</?[A-Za-z][^\s/>]*(?:[ \t][^>]*)?/?>[ \t]*$"
)
HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
HTML_TAG_RE = re.compile(r"</?[A-Za-z][A-Za-z0-9-]*(?:[ \t/][^>]*)?>")
A_START_TAG_RE = re.compile(r"<a\s[^>]*>", re.IGNORECASE)
A_ANCHOR_ATTRIBUTE_RE = re.compile(
    r"""(?<![\w-])(?:id|name)[ \t]*=[ \t]*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""",
    re.IGNORECASE,
)
BACKSLASH_ESCAPE_RE = re.compile(r"\\([!-/:-@\[-`{-~])")
IMAGE_RE = re.compile(r"!\[[^\]]*\]\([^)]*\)")
INLINE_LINK_MARKUP_RE = re.compile(r"\[([^\]]*)\]\([^)]*\)")
REFERENCE_LINK_MARKUP_RE = re.compile(r"\[([^\]]*)\]\[[^\]]*\]")
CODE_SPAN_RE = re.compile(r"(`+)([^`].*?)\1")
CODE_PLACEHOLDER_RE = re.compile(r"\x00(\d+)\x00")
UNDERSCORE_EMPHASIS_RE = re.compile(r"(?<!\w)(_{1,3})(?=\S)(.+?)(?<=\S)\1(?!\w)")


class LinkCheckError(RuntimeError):
    """The checker could not be exercised safely."""


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


def mask_fences(text: str) -> str:
    """Mask fenced code blocks while preserving line numbers."""
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

        masked_lines.append(line)

    return "".join(masked_lines)


def mask_code(text: str) -> str:
    """Mask fenced and inline code while preserving line numbers."""
    return re.sub(r"`[^`\n]*`", lambda m: " " * len(m.group(0)), mask_fences(text))


def iter_links(text: str) -> list[tuple[int, str]]:
    links: list[tuple[int, str]] = []
    for regex in (INLINE_LINK_RE, REF_LINK_RE):
        for match in regex.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            target = split_target(match.group(1))
            if target:
                links.append((line, target))
    return links


def heading_text(markup: str) -> str:
    """Reduce heading markup to the plain text GitHub derives an anchor from."""
    code_spans: list[str] = []

    def park_code_span(match: re.Match[str]) -> str:
        # Code content is literal: park it so that link, tag, and emphasis
        # markup inside it is not rewritten.
        content = match.group(2)
        if content.startswith(" ") and content.endswith(" ") and content.strip(" "):
            content = content[1:-1]
        code_spans.append(content)
        return f"\x00{len(code_spans) - 1}\x00"

    text = CODE_SPAN_RE.sub(park_code_span, markup)
    text = BACKSLASH_ESCAPE_RE.sub(r"\1", text)
    text = HTML_TAG_RE.sub("", HTML_COMMENT_RE.sub("", text))
    text = html.unescape(text)
    # Image alternatives are not text content; link labels are.
    text = IMAGE_RE.sub("", text)
    text = INLINE_LINK_MARKUP_RE.sub(r"\1", text)
    text = REFERENCE_LINK_MARKUP_RE.sub(r"\1", text)
    text = UNDERSCORE_EMPHASIS_RE.sub(r"\2", text)
    return CODE_PLACEHOLDER_RE.sub(lambda m: code_spans[int(m.group(1))], text)


def slugify(text: str) -> str:
    """Derive the GitHub anchor from plain heading text.

    Lowercase, drop everything except word characters and hyphens, and turn
    each space into a hyphen. Word characters keep letters (Chinese included),
    digits, combining marks, and connector punctuation such as ``_``.
    """
    slug: list[str] = []
    for char in text.lower():
        category = unicodedata.category(char)
        if char == " ":
            slug.append("-")
        elif char == "-" or category[0] in "LM" or category in ("Nd", "Pc"):
            slug.append(char)
    return "".join(slug)


def iter_headings(text: str) -> list[str]:
    """Return the markup of every ATX and Setext heading outside fences."""
    headings: list[str] = []
    paragraph: list[str] = []
    paragraph_depth = 0
    for line in text.split("\n"):
        stripped = line
        depth = 0
        while match := CONTAINER_PREFIX_RE.match(stripped):
            stripped = stripped[match.end() :]
            depth += 1

        if not stripped.strip():
            paragraph = []
            continue

        match = ATX_HEADING_RE.match(stripped)
        if match:
            headings.append(ATX_CLOSING_RE.sub("", match.group(2) or "").strip())
            paragraph = []
            continue

        underline = SETEXT_UNDERLINE_RE.match(stripped)
        if underline:
            # The underline closes the paragraph only at its own container
            # depth (CommonMark 4.6).
            if paragraph and depth == paragraph_depth:
                headings.append(" ".join(paragraph))
                paragraph = []
                continue
            if underline.group(1)[0] == "-":
                # A dash line that closes no paragraph is a thematic break.
                paragraph = []
                continue
            if paragraph and depth < paragraph_depth:
                # A lazy equals line is ordinary paragraph text.
                paragraph.append(stripped)
                continue

        # Only a paragraph line can continue into a Setext heading. A deeper
        # line opens a new block and starts a new paragraph; a shallower line
        # is lazy continuation text of the current one.
        if NON_PARAGRAPH_RE.match(stripped):
            paragraph = []
        elif not paragraph or depth > paragraph_depth:
            paragraph = [stripped.strip()]
            paragraph_depth = depth
        else:
            paragraph.append(stripped.strip())

    return headings


def document_anchors(text: str) -> frozenset[str]:
    """Collect the explicit ``<a>`` anchors and heading anchors of a document."""
    body = mask_fences(text)
    anchors: set[str] = set()
    for tag in A_START_TAG_RE.findall(body):
        attribute = A_ANCHOR_ATTRIBUTE_RE.search(tag)
        if attribute:
            value = next(group for group in attribute.groups() if group is not None)
            if value:
                anchors.add(value)

    # GitHub suffixes the n-th repeat of a slug with "-n", starting at 1.
    occurrences: dict[str, int] = {}
    for markup in iter_headings(body):
        slug = slugify(heading_text(markup))
        if not slug:
            continue
        count = occurrences.get(slug, 0)
        occurrences[slug] = count + 1
        anchors.add(f"{slug}-{count}" if count else slug)
    return frozenset(anchors)


def markdown_anchors(path: Path, cache: dict[Path, frozenset[str]]) -> frozenset[str] | None:
    """Return the anchors of a Markdown file, or ``None`` when anchors do not apply."""
    if path.suffix.lower() != ".md":
        return None
    if path not in cache:
        try:
            cache[path] = document_anchors(path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError):
            return None
    return cache[path]


def check_link(
    repo: Path, source: Path, line: int, target: str, anchor_cache: dict[Path, frozenset[str]]
) -> str | None:
    if is_external(target):
        return None

    path_part, _, fragment = target.partition("#")
    path_part = unquote(path_part)
    if path_part:
        candidate = (source.parent / path_part).resolve()
        try:
            candidate.relative_to(repo)
        except ValueError:
            return f"{source.relative_to(repo)}:{line}: link escapes repository: {target}"

        if not candidate.exists():
            return f"{source.relative_to(repo)}:{line}: missing local link target: {target}"
    else:
        candidate = source

    if not fragment:
        return None
    anchors = markdown_anchors(candidate, anchor_cache)
    if anchors is not None and unquote(fragment) not in anchors:
        return f"{source.relative_to(repo)}:{line}: missing link anchor: {target}"

    return None


def scan(repo: Path, sources: list[Path]) -> list[str]:
    """Return one message per broken local link in ``sources``."""
    anchor_cache: dict[Path, frozenset[str]] = {}
    failures: list[str] = []

    for source in sources:
        text = mask_code(source.read_text(encoding="utf-8"))
        for line, target in iter_links(text):
            failure = check_link(repo, source, line, target, anchor_cache)
            if failure:
                failures.append(failure)

    return failures


def run_self_test() -> None:
    """Run the checker on a temporary tree.

    Every link in ``valid.md`` must pass, and every broken link in
    ``broken.md`` must be reported with its own line number.
    """
    with tempfile.TemporaryDirectory(prefix="gnfs-markdown-links-") as temp_directory:
        root = Path(temp_directory).resolve()
        docs = root / "docs"
        docs.mkdir()
        docs.joinpath("flags.md").write_text(
            "---\n"
            "name: flags\n"
            "---\n"
            '<a id="legacy"></a>\n'
            "<a name='named' href=\"x\">named</a>\n"
            "\n"
            "# 环境变量与开关\n"
            "\n"
            "## The `GNFS_V0_BFS` policy (size-aware): default ON\n"
            "\n"
            "## 概述\n"
            "\n"
            "## 概述\n"
            "\n"
            "## 概述!\n"
            "\n"
            "> ### 引用里的标题\n"
            "\n"
            "1. 列表项\n"
            "\n"
            "   ### 列表里的标题\n"
            "\n"
            "跨行的\n"
            "Setext 标题\n"
            "---\n"
            "\n"
            "> 引用里的段落\n"
            "---\n"
            "\n"
            "> foo\n"
            "bar\n"
            "===\n"
            "\n"
            "普通段落\n"
            "> 引用里的段落二\n"
            "> ---\n"
            "\n"
            "<b>行内标签</b> 段落\n"
            "---\n"
            "\n"
            "```text\n"
            "## 代码块里的标题\n"
            "```\n"
            "\n"
            "- 列表项不是标题\n"
            "---\n"
            "\n"
            "## 全角标点：中文（标题）\n"
            "\n"
            "## [链接文字](README.md) 与 **加粗**\n"
            "\n"
            "## 使用 `std::vector<int>` 与 `[x](y)`\n",
            encoding="utf-8",
        )
        root.joinpath("data.txt").write_text("1\n2\n", encoding="utf-8")
        root.joinpath("valid.md").write_text(
            "- [中文](docs/flags.md#环境变量与开关)\n"
            "- [元数据段落](docs/flags.md#name-flags)\n"
            "- [编码](docs/flags.md#%E6%A6%82%E8%BF%B0-1)\n"
            "- [代码与标点](docs/flags.md#the-gnfs_v0_bfs-policy-size-aware-default-on)\n"
            "- [重复](docs/flags.md#概述-2)\n"
            "- [显式 id](docs/flags.md#legacy)\n"
            "- [显式 name](docs/flags.md#named)\n"
            "- [引用块](docs/flags.md#引用里的标题)\n"
            "- [列表里的标题](docs/flags.md#列表里的标题)\n"
            "- [Setext](docs/flags.md#跨行的-setext-标题)\n"
            "- [引用块里的 Setext](docs/flags.md#引用里的段落二)\n"
            "- [行内 HTML](docs/flags.md#行内标签-段落)\n"
            "- [行内标记](docs/flags.md#链接文字-与-加粗)\n"
            "- [代码里的标记](docs/flags.md#使用-stdvectorint-与-xy)\n"
            "- [同文件](#本地)\n"
            "- [无锚点](docs/flags.md)\n"
            "- [非 Markdown](data.txt#L10)\n"
            "- [外链](https://example.com/missing#anchor)\n"
            "- [引用式][ref]\n"
            "- 行内代码里的 `[fake](docs/flags.md#不存在)` 不算链接\n"
            "\n"
            "[ref]: docs/flags.md#全角标点中文标题\n"
            "\n"
            "## 本地\n",
            encoding="utf-8",
        )
        root.joinpath("broken.md").write_text(
            "[跨文件](docs/flags.md#不存在的标题)\n"
            "[同文件](#不存在的标题)\n"
            "[代码块](docs/flags.md#代码块里的标题)\n"
            "[大小写](docs/flags.md#Legacy)\n"
            "[引用下的横线](docs/flags.md#引用里的段落)\n"
            "[惰性续行](docs/flags.md#foo-bar)\n"
            "[惰性续行末行](docs/flags.md#bar)\n"
            "[跨块拼接](docs/flags.md#普通段落-引用里的段落二)\n"
            "[列表项](docs/flags.md#列表项不是标题)\n"
            "[开头的横线](docs/flags.md#---name-flags)\n"
            "[被改写的代码](docs/flags.md#使用-stdvector-与-x)\n"
            "[文件缺失](docs/missing.md#anchor)\n"
            "\n"
            "[引用式][ref]\n"
            "\n"
            "[ref]: docs/flags.md#概述-3\n",
            encoding="utf-8",
        )
        sources = [root / "valid.md", root / "broken.md"]

        expected = [
            "broken.md:1: missing link anchor: docs/flags.md#不存在的标题",
            "broken.md:2: missing link anchor: #不存在的标题",
            "broken.md:3: missing link anchor: docs/flags.md#代码块里的标题",
            "broken.md:4: missing link anchor: docs/flags.md#Legacy",
            "broken.md:5: missing link anchor: docs/flags.md#引用里的段落",
            "broken.md:6: missing link anchor: docs/flags.md#foo-bar",
            "broken.md:7: missing link anchor: docs/flags.md#bar",
            "broken.md:8: missing link anchor: docs/flags.md#普通段落-引用里的段落二",
            "broken.md:9: missing link anchor: docs/flags.md#列表项不是标题",
            "broken.md:10: missing link anchor: docs/flags.md#---name-flags",
            "broken.md:11: missing link anchor: docs/flags.md#使用-stdvector-与-x",
            "broken.md:12: missing local link target: docs/missing.md#anchor",
            "broken.md:16: missing link anchor: docs/flags.md#概述-3",
        ]
        failures = scan(root, sources)
        if failures != expected:
            raise LinkCheckError(f"self-test expected {expected}, found {failures}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run checker regression tests")
    args = parser.parse_args()

    try:
        if args.self_test:
            run_self_test()
            print("[PASS] Markdown link checker self-test")
            return 0

        repo = Path.cwd().resolve()
        failures = scan(repo, tracked_markdown_files(repo))
    except (LinkCheckError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    if failures:
        print("Broken local Markdown links:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Local Markdown links are valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
