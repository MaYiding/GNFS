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
# A reference definition may put its destination on the line after the label.
REF_LINK_RE = re.compile(r"^[ \t]*\[[^\]]+\]:[ \t]*(\S+|\n[ \t]*\S+)", re.MULTILINE)
INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
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
FRONT_MATTER_DELIMITER_RE = re.compile(r"(?:---|\.\.\.)[ \t]*")
# The first content line of YAML front matter is a mapping key.
YAML_KEY_RE = re.compile(r"[^\s#][^:]*:(?:\s|$)")
# Tags that open an HTML block in CommonMark (start condition 6).
HTML_BLOCK_TAGS = (
    "address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|"
    "details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|"
    "h[1-6]|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|noframes|ol|"
    "optgroup|option|p|param|search|section|summary|table|tbody|td|tfoot|th|thead|title|tr|"
    "track|ul"
)
# Lines that cannot be the text of a Setext heading: indented code, list
# items, table rows, and HTML blocks (a block-level tag, a tag alone on the
# line, or a comment, processing instruction, or declaration opener).
NON_PARAGRAPH_RE = re.compile(
    r"^ {4}|^[-*+][ \t]|^\d{1,9}[.)][ \t]|^\||^<!--|^<[!?]"
    r"|^</?[A-Za-z][^\s/>]*(?:[ \t][^>]*)?/?>[ \t]*$"
    rf"|^</?(?:{HTML_BLOCK_TAGS})(?=[\s/>]|$)",
    re.IGNORECASE,
)
HTML_HEADING_RE = re.compile(r"<h([1-6])\b[^>]*>(.*?)</h\1\s*>", re.IGNORECASE | re.DOTALL)
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


def mask_front_matter(text: str) -> str:
    """Mask YAML front matter while preserving line numbers.

    GitHub renders a leading ``---`` block as a table instead of Markdown, so
    none of its lines becomes a heading. Only a block that opens with a YAML
    mapping key counts; otherwise, or without a closing delimiter, the leading
    ``---`` is an ordinary thematic break.
    """
    lines = text.split("\n")
    if not lines[0].startswith("-") or not FRONT_MATTER_DELIMITER_RE.fullmatch(lines[0]):
        return text
    first = next((line for line in lines[1:] if line.strip() and not line.lstrip().startswith("#")), "")
    if not YAML_KEY_RE.match(first):
        return text
    for index in range(1, len(lines)):
        if FRONT_MATTER_DELIMITER_RE.fullmatch(lines[index]):
            return "\n".join([""] * (index + 1) + lines[index + 1 :])
    return text


def mask_code(text: str) -> str:
    """Mask fenced and inline code while preserving line numbers."""
    return INLINE_CODE_RE.sub(lambda m: " " * len(m.group(0)), mask_fences(text))


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

    Lowercase, drop everything except word characters, hyphens, and spaces,
    and turn each space into a hyphen. A line break inside the heading text is
    dropped rather than turned into a hyphen. Word characters keep letters
    (Chinese included), digits, combining marks, and connector punctuation
    such as ``_``.
    """
    slug: list[str] = []
    for char in text.lower():
        category = unicodedata.category(char)
        if char == " ":
            slug.append("-")
        elif char == "-" or category[0] in "LM" or category in ("Nd", "Pc"):
            slug.append(char)
    return "".join(slug)


def iter_headings(text: str) -> list[tuple[int, str]]:
    """Return ``(offset, markup)`` for every ATX and Setext heading outside fences.

    The offset is the position of the heading's first line in ``text``.
    """
    headings: list[tuple[int, str]] = []
    paragraph: list[str] = []
    paragraph_depth = 0
    paragraph_offset = 0
    offset = 0
    for line in text.split("\n"):
        line_offset = offset
        offset += len(line) + 1
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
            markup = ATX_CLOSING_RE.sub("", match.group(2) or "").strip()
            headings.append((line_offset, markup))
            paragraph = []
            continue

        underline = SETEXT_UNDERLINE_RE.match(stripped)
        if underline:
            # The underline closes the paragraph only at its own container
            # depth (CommonMark 4.6).
            if paragraph and depth == paragraph_depth:
                # GitHub keeps the line break in the heading text and then
                # drops it from the anchor, so the lines are not joined by a space.
                headings.append((paragraph_offset, "\n".join(paragraph)))
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
            paragraph_offset = line_offset
        else:
            paragraph.append(stripped.strip())

    return headings


def html_heading_text(inner: str) -> str:
    """Reduce the inner HTML of a heading element to its text content."""
    return html.unescape(HTML_TAG_RE.sub("", HTML_COMMENT_RE.sub("", inner)))


def document_anchors(text: str) -> frozenset[str]:
    """Collect the explicit ``<a>`` anchors and heading anchors of a document."""
    body = mask_fences(mask_front_matter(text))
    code_spans = [match.span() for match in INLINE_CODE_RE.finditer(body)]

    def in_code(position: int) -> bool:
        return any(start < position < end for start, end in code_spans)

    anchors: set[str] = set()
    for match in A_START_TAG_RE.finditer(body):
        attribute = A_ANCHOR_ATTRIBUTE_RE.search(match.group(0))
        if attribute and not in_code(match.start()):
            value = next(group for group in attribute.groups() if group is not None)
            if value:
                anchors.add(value)

    # Markdown and raw HTML headings share one numbering, in document order.
    plain = [(offset, heading_text(markup)) for offset, markup in iter_headings(body)]
    for match in HTML_HEADING_RE.finditer(body):
        if in_code(match.start()):
            continue
        # Markdown inside an HTML block is not parsed.
        plain = [item for item in plain if not match.start() < item[0] < match.end()]
        plain.append((match.start(), html_heading_text(match.group(2))))

    # GitHub suffixes the n-th repeat of a slug with "-n", starting at 1.
    occurrences: dict[str, int] = {}
    for _, heading in sorted(plain):
        slug = slugify(heading)
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
        (root / "docs").mkdir()

        def write(relative: str, *lines: str) -> None:
            root.joinpath(relative).write_text("\n".join(lines) + "\n", encoding="utf-8")

        write(
            "docs/flags.md",
            "---",
            "name: flags",
            "---",
            '<a id="legacy"></a>',
            "<a name='named' href=\"x\">named</a>",
            "",
            '<h1 align="center">GNFS</h1>',
            "",
            "# 环境变量与开关",
            "",
            "## The `GNFS_V0_BFS` policy (size-aware): default ON",
            "",
            "## 概述",
            "",
            "## 概述",
            "",
            "## 概述!",
            "",
            "> ### 引用里的标题",
            "",
            "1. 列表项",
            "",
            "   ### 列表里的标题",
            "",
            "跨行的",
            "Setext 标题",
            "---",
            "",
            "> 引用里的段落",
            "---",
            "",
            "> foo",
            "bar",
            "===",
            "",
            "普通段落",
            "> 引用里的段落二",
            "> ---",
            "",
            "<b>行内标签</b> 段落",
            "---",
            "",
            "<h3>Q&amp;A</h3>",
            "---",
            "",
            "<h2>",
            "  多行 HTML 标题",
            "</h2>",
            "",
            "## GNFS",
            "",
            "```text",
            "## 代码块里的标题",
            "```",
            "",
            "- 列表项不是标题",
            "---",
            "",
            "## 全角标点：中文（标题）",
            "",
            "## [链接文字](README.md) 与 **加粗**",
            "",
            "## 使用 `std::vector<int>` 与 `[x](y)`",
            "",
            "`<h4>代码里的标题</h4>`",
            "",
            '`<a id="in-code"></a>`',
        )
        # A leading "---" without a closing delimiter is a thematic break.
        write("docs/thematic.md", "---", "标题", "===")
        # A leading "---" that opens on a heading is not front matter either,
        # and neither is a YAML block that never closes.
        write("docs/rule.md", "---", "## 第一个标题", "", "...", "", "## 第二个标题")
        write("docs/unterminated.md", "---", "name: unterminated", "", "## 之后的标题")
        write("data.txt", "1", "2")
        write(
            "valid.md",
            "- [中文](docs/flags.md#环境变量与开关)",
            "- [编码](docs/flags.md#%E6%A6%82%E8%BF%B0-1)",
            "- [代码与标点](docs/flags.md#the-gnfs_v0_bfs-policy-size-aware-default-on)",
            "- [重复](docs/flags.md#概述-2)",
            "- [显式 id](docs/flags.md#legacy)",
            "- [显式 name](docs/flags.md#named)",
            "- [引用块](docs/flags.md#引用里的标题)",
            "- [列表里的标题](docs/flags.md#列表里的标题)",
            "- [Setext](docs/flags.md#跨行的setext-标题)",
            "- [引用块里的 Setext](docs/flags.md#引用里的段落二)",
            "- [行内 HTML](docs/flags.md#行内标签-段落)",
            "- [HTML 标题](docs/flags.md#gnfs)",
            "- [HTML 标题之后的重复](docs/flags.md#gnfs-1)",
            "- [HTML 实体](docs/flags.md#qa)",
            "- [多行 HTML 标题](docs/flags.md#--多行-html-标题)",
            "- [开头的横线](docs/thematic.md#标题)",
            "- [横线后的标题](docs/rule.md#第一个标题)",
            "- [省略号后的标题](docs/rule.md#第二个标题)",
            "- [未闭合元数据之后的标题](docs/unterminated.md#之后的标题)",
            "- [行内标记](docs/flags.md#链接文字-与-加粗)",
            "- [代码里的标记](docs/flags.md#使用-stdvectorint-与-xy)",
            "- [同文件](#本地)",
            "- [无锚点](docs/flags.md)",
            "- [非 Markdown](data.txt#L10)",
            "- [外链](https://example.com/missing#anchor)",
            "- [引用式][ref]",
            "- [跨行引用式][ref-next]",
            "- 行内代码里的 `[fake](docs/flags.md#不存在)` 不算链接",
            "",
            "[ref]: docs/flags.md#全角标点中文标题",
            "",
            "[ref-next]:",
            "  docs/flags.md#全角标点中文标题",
            "",
            "## 本地",
        )
        write(
            "broken.md",
            "[跨文件](docs/flags.md#不存在的标题)",
            "[同文件](#不存在的标题)",
            "[代码块](docs/flags.md#代码块里的标题)",
            "[大小写](docs/flags.md#Legacy)",
            "[引用下的横线](docs/flags.md#引用里的段落)",
            "[惰性续行](docs/flags.md#foo-bar)",
            "[惰性续行末行](docs/flags.md#bar)",
            "[跨块拼接](docs/flags.md#普通段落-引用里的段落二)",
            "[列表项](docs/flags.md#列表项不是标题)",
            "[元数据段落](docs/flags.md#name-flags)",
            "[未闭合的元数据段落](docs/unterminated.md#name-unterminated)",
            "[横线拼进标题](docs/thematic.md#----标题)",
            "[代码里的 HTML 标题](docs/flags.md#代码里的标题)",
            "[代码里的显式锚点](docs/flags.md#in-code)",
            "[HTML 重复错位](docs/flags.md#gnfs-2)",
            "[HTML 块里的横线](docs/flags.md#qa-1)",
            "[跨行 Setext 的连字符](docs/flags.md#跨行的-setext-标题)",
            "[去掉缩进的 HTML 标题](docs/flags.md#多行-html-标题)",
            "[被改写的代码](docs/flags.md#使用-stdvector-与-x)",
            "[文件缺失](docs/missing.md#anchor)",
            "",
            "[引用式][ref]",
            "",
            "[ref]: docs/flags.md#概述-3",
            "",
            "[跨行引用式][ref-next]",
            "",
            "[ref-next]:",
            "  docs/flags.md#概述-4",
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
            "broken.md:10: missing link anchor: docs/flags.md#name-flags",
            "broken.md:11: missing link anchor: docs/unterminated.md#name-unterminated",
            "broken.md:12: missing link anchor: docs/thematic.md#----标题",
            "broken.md:13: missing link anchor: docs/flags.md#代码里的标题",
            "broken.md:14: missing link anchor: docs/flags.md#in-code",
            "broken.md:15: missing link anchor: docs/flags.md#gnfs-2",
            "broken.md:16: missing link anchor: docs/flags.md#qa-1",
            "broken.md:17: missing link anchor: docs/flags.md#跨行的-setext-标题",
            "broken.md:18: missing link anchor: docs/flags.md#多行-html-标题",
            "broken.md:19: missing link anchor: docs/flags.md#使用-stdvector-与-x",
            "broken.md:20: missing local link target: docs/missing.md#anchor",
            "broken.md:24: missing link anchor: docs/flags.md#概述-3",
            "broken.md:28: missing link anchor: docs/flags.md#概述-4",
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
