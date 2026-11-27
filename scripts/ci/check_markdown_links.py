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

# Block patterns match the content of a line: what remains after the markers of
# its block quotes and list items and up to three spaces of indentation.
TAB_WIDTH = 4
ATX_HEADING_RE = re.compile(r"(#{1,6})(?:[ ]+(.*?))?[ ]*$")
ATX_CLOSING_RE = re.compile(r"[ ]+#+$")
SETEXT_UNDERLINE_RE = re.compile(r"(=+|-+)[ ]*$")
THEMATIC_BREAK_RE = re.compile(r"([-*_])(?:[ ]*\1){2,}[ ]*$")
# An opening fence carries an info string, which may not contain a backtick
# when the fence is made of backticks. A closing fence carries nothing else.
FENCE_OPEN_RE = re.compile(r"(?P<marker>`{3,}|~{3,})(?P<info>.*)$")
FENCE_CLOSE_RE = re.compile(r"(?P<marker>`{3,}|~{3,})[ ]*$")
LIST_MARKER_RE = re.compile(r"(?:(?P<bullet>[-*+])|(?P<number>\d{1,9})[.)])(?= |$)")
TABLE_DELIMITER_RE = re.compile(r"\|?[ ]*:?-+:?[ ]*(?:\|[ ]*:?-+:?[ ]*)*\|?[ ]*$")
# CommonMark HTML blocks: start conditions 1 to 5 end at a string, 6 and 7 at
# a blank line. Condition 7 cannot interrupt a paragraph and excludes open
# tags named script, style, or pre, which condition 1 takes instead.
HTML_BLOCK_STARTS = (
    (
        re.compile(r"<(?i:script|pre|style|textarea)(?=[ />]|$)"),
        re.compile(r"(?i)</(?:script|pre|style|textarea)>"),
    ),
    (re.compile(r"<!--"), re.compile(r"-->")),
    (re.compile(r"<\?"), re.compile(r"\?>")),
    (re.compile(r"<![A-Za-z]"), re.compile(r">")),
    (re.compile(r"<!\[CDATA\["), re.compile(r"\]\]>")),
)
# Tags that open an HTML block in CommonMark (start condition 6).
HTML_BLOCK_TAGS = (
    "address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|"
    "details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|"
    "h[1-6]|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|noframes|ol|"
    "optgroup|option|p|param|search|section|summary|table|tbody|td|tfoot|th|thead|title|tr|"
    "track|ul"
)
HTML_BLOCK_TAG_RE = re.compile(rf"</?(?:{HTML_BLOCK_TAGS})(?=[ />]|$)", re.IGNORECASE)
# A complete closing tag of any name, or a complete open tag of any other
# name (start condition 7).
HTML_COMPLETE_TAG_RE = re.compile(
    r"""(?:</[A-Za-z][^\s/>]*[ ]*>"""
    r"""|<(?!(?i:script|style|pre)(?=[ />]|$))[A-Za-z][^\s/>]*"""
    r"""(?:[ ]+[^\s"'=<>`]+(?:[ ]*=[ ]*(?:"[^"]*"|'[^']*'|[^\s"'=<>`]+))?)*[ ]*/?>)[ ]*$"""
)
LINK_LABEL = r"\[(?:\\.|[^\]\\])+\]"
LINK_REFERENCE_RE = re.compile(
    "^" + LINK_LABEL + r":[ ]*(?:<[^<>]*>|(?:\\.|[^\s\\])+)"
    r"""(?:[ ]+(?P<title>"[^"]*"|'[^']*'|\([^()]*\)))?[ ]*$"""
)
LINK_LABEL_LINE_RE = re.compile("^" + LINK_LABEL + r":[ ]*$")
LINK_DESTINATION_LINE_RE = re.compile(r"^(?:<[^<>]*>|(?:\\.|[^\s\\])+)[ ]*$")
LINK_TITLE_LINE_RE = re.compile(r"""^(?:"[^"]*"|'[^']*'|\([^()]*\))[ ]*$""")
FOOTNOTE_DEFINITION_RE = re.compile(r"^\[\^[^\]]+\]:")
FRONT_MATTER_DELIMITER_RE = re.compile(r"(?:---|\.\.\.)[ \t]*")
# The first content line of YAML front matter is a mapping key.
YAML_KEY_RE = re.compile(r"[^\s#][^:]*:(?:\s|$)")
# A raw heading element runs to its closing tag, to the next heading tag or
# block end tag, either of which closes it, or to the end of the document.
HTML_HEADING_RE = re.compile(
    rf"<h([1-6])\b[^>]*>(.*?)(?=</h\1\s*>|<h[1-6]\b|</(?:{HTML_BLOCK_TAGS})(?=[ />]|$)|\Z)"
    r"(?:</h\1\s*>)?",
    re.IGNORECASE | re.DOTALL,
)
# A comment that is never closed swallows the rest of the document.
HTML_COMMENT_RE = re.compile(r"<!--.*?(?:-->|\Z)", re.DOTALL)
HTML_TAG_RE = re.compile(r"</?[A-Za-z][A-Za-z0-9-]*(?:[ \t/][^>]*)?>")
HTML_START_TAG_RE = re.compile(r"<([A-Za-z][^\s/>]*)((?:\s[^>]*)?)>")
ID_ATTRIBUTE_RE = re.compile(
    r"""(?<![\w-])(id|name)[ \t]*=[ \t]*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""", re.IGNORECASE
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


def expand_tabs(line: str) -> tuple[str, list[int]]:
    """Return the line with tabs turned into spaces, and a column-to-index map."""
    expanded: list[str] = []
    origin: list[int] = []
    for index, char in enumerate(line):
        if char == "\t":
            width = TAB_WIDTH - len(expanded) % TAB_WIDTH
            expanded.append(" " * width)
            origin.extend([index] * width)
        else:
            expanded.append(char)
            origin.append(index)
    origin.append(len(line))
    return "".join(expanded), origin


def leading_spaces(text: str, start: int) -> int:
    """Count the spaces that begin ``text`` at ``start``."""
    end = start
    while end < len(text) and text[end] == " ":
        end += 1
    return end - start


def html_block_start(content: str, in_paragraph: bool) -> int:
    """Return the CommonMark start condition that ``content`` meets, or 0."""
    for condition, (start, _) in enumerate(HTML_BLOCK_STARTS, start=1):
        if start.match(content):
            return condition
    if HTML_BLOCK_TAG_RE.match(content):
        return 6
    if not in_paragraph and HTML_COMPLETE_TAG_RE.match(content):
        return 7
    return 0


def is_block_start(indent: int, content: str) -> bool:
    """Tell whether a line starts a block, so it cannot continue a paragraph.

    Indented code cannot interrupt a paragraph, and a Setext underline that
    would have to continue one lazily turns into plain text.
    """
    if indent >= 4:
        return False
    opening = FENCE_OPEN_RE.match(content)
    if (
        content.startswith(">")
        or ATX_HEADING_RE.match(content) is not None
        or THEMATIC_BREAK_RE.match(content) is not None
        or LIST_MARKER_RE.match(content) is not None
        or (
            opening is not None
            and not (opening.group("marker")[0] == "`" and "`" in opening.group("info"))
        )
        # An HTML block of type 7 cannot interrupt a paragraph, but it does
        # start one in a container the paragraph does not reach.
        or html_block_start(content, False) > 0
        or FOOTNOTE_DEFINITION_RE.match(content) is not None
    ):
        return True
    # A Setext underline that would have to continue the paragraph lazily is
    # plain text, like any other line.
    return False


def table_cells(row: str) -> int:
    """Count the cells of a table row."""
    row = row.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|"):
        row = row[:-1]
    return len(re.split(r"(?<!\\)\|", row))


def without_definitions(lines: list[str]) -> list[str]:
    """Drop the link reference definitions that open a paragraph.

    A definition is a label line, with its destination on the same line or on
    the next one, and an optional title on the line after the destination.
    """
    index = 0
    while index < len(lines):
        reference = LINK_REFERENCE_RE.match(lines[index])
        if reference is not None:
            index += 1
            if not reference.group("title") and index < len(lines) and LINK_TITLE_LINE_RE.match(lines[index]):
                index += 1
        elif (
            LINK_LABEL_LINE_RE.match(lines[index])
            and index + 1 < len(lines)
            and LINK_DESTINATION_LINE_RE.match(lines[index + 1])
        ):
            index += 2
            if index < len(lines) and LINK_TITLE_LINE_RE.match(lines[index]):
                index += 1
        else:
            break
    return lines[index:]


def scan_blocks(text: str) -> tuple[list[tuple[int, int, str]], str]:
    """Find the Markdown headings of a document and mask its fenced code.

    Return ``((offset, level, markup), ...)`` for every ATX and Setext heading
    in document order, ``offset`` being the position of the heading's first
    line, and a copy of ``text`` in which every line of fenced code, markers
    included, is blanked to spaces, so that positions still line up.

    Block quotes and list items are followed the way CommonMark matches them,
    so a heading indented to the content column of a list item is found, and a
    line that leaves a container is not taken for part of it.
    """
    headings: list[tuple[int, int, str]] = []
    masked: list[str] = []
    containers: list[tuple[bool, int]] = []  # (is block quote, content indent)
    fence: tuple[str, int, int] | None = None  # marker character, marker length, depth
    html_end: tuple[re.Pattern[str] | None, int] | None = None
    paragraph: tuple[list[str], int, int] | None = None  # lines, depth, offset
    table_depth = -1
    note_depth = -1
    offset = 0

    for line in text.split("\n"):
        line_offset = offset
        offset += len(line) + 1
        expanded, origin = expand_tabs(line)

        # Take off the markers of the containers the line belongs to. A line
        # is blank when nothing follows them, so "> " ends the paragraph of
        # its block quote.
        col = 0
        matched = 0
        while matched < len(containers) and expanded[col:].strip():
            is_quote, indent = containers[matched]
            spaces = leading_spaces(expanded, col)
            if is_quote:
                if spaces > 3 or expanded[col + spaces : col + spaces + 1] != ">":
                    break
                col += spaces + 1
                if expanded[col : col + 1] == " ":
                    col += 1
            elif spaces < indent:
                break
            else:
                col += indent
            matched += 1
        blank = not expanded[col:].strip()
        depth = len(containers)
        indent = leading_spaces(expanded, col)
        content = expanded[col + indent :]
        start = line_offset + origin[col + indent]

        # A line that leaves a container closes it and its open blocks, unless
        # it is blank or lazily continues a paragraph.
        if matched < depth and not blank:
            if (
                paragraph is not None
                and paragraph[1] > matched
                and not is_block_start(indent, content)
            ):
                paragraph[0].append(line[origin[col + indent] :].strip())
                masked.append(line)
                continue
            fence = None if fence is not None and fence[2] > matched else fence
            html_end = None if html_end is not None and html_end[1] > matched else html_end
            paragraph = None if paragraph is not None and paragraph[1] > matched else paragraph
            table_depth = -1 if table_depth > matched else table_depth
            note_depth = -1 if note_depth > matched else note_depth
            containers = containers[:matched]
            depth = matched

        if fence is not None:
            closing = FENCE_CLOSE_RE.match(content) if indent <= 3 else None
            marker = closing.group("marker") if closing else ""
            if marker and marker[0] == fence[0] and len(marker) >= fence[1]:
                fence = None
            masked.append(" " * len(line))
            continue

        if blank:
            paragraph = None
            table_depth = note_depth = -1
            if html_end is not None and html_end[0] is None:
                html_end = None
            masked.append(line)
            continue

        # The remaining lines of an HTML block are not Markdown.
        if html_end is not None and html_end[1] == depth:
            if html_end[0] is not None and html_end[0].search(expanded, col):
                html_end = None
            masked.append(line)
            continue

        # A footnote definition and a table both run until a blank line or
        # the start of another block, and neither holds a heading.
        ends = is_block_start(indent, content)
        if note_depth == depth and not ends:
            masked.append(line)
            continue
        note_depth = -1
        if table_depth == depth and not ends:
            masked.append(line)
            continue
        table_depth = -1

        # Open the containers the line starts. A thematic break wins over the
        # list item it could also be read as, and an empty list item or one
        # that does not start at 1 cannot interrupt a paragraph.
        while indent <= 3 and not THEMATIC_BREAK_RE.match(content):
            marker = None if content.startswith(">") else LIST_MARKER_RE.match(content)
            if content.startswith(">"):
                col += indent + 1
                if expanded[col : col + 1] == " ":
                    col += 1
                containers.append((True, 0))
            elif marker is not None and (
                paragraph is None
                or (bool(content[marker.end() :].strip()) and marker.group("number") in (None, "1"))
            ):
                # The content starts after the marker and the spaces that follow
                # it, or one column in for an empty item or for indented code.
                marker_end = col + indent + marker.end()
                after = leading_spaces(expanded, marker_end)
                step = 1 if after > 4 or not expanded[marker_end:].strip() else after
                containers.append((False, marker_end + step - col))
                col = min(marker_end + step, len(expanded))
            else:
                break
            paragraph = None
            depth += 1
            indent = leading_spaces(expanded, col)
            content = expanded[col + indent :]
            start = line_offset + origin[col + indent]

        if not content:
            # Nothing follows the markers, as in an empty list item.
            table_depth = note_depth = -1
            masked.append(line)
            continue

        if indent >= 4:
            # Indented code, or the continuation of a paragraph.
            if paragraph is not None:
                paragraph[0].append(line[origin[col + indent] :].strip())
            masked.append(line)
            continue

        plain = line[origin[col + indent] :].strip()
        atx = ATX_HEADING_RE.match(content)
        opening = FENCE_OPEN_RE.match(content)
        if opening and opening.group("marker")[0] == "`" and "`" in opening.group("info"):
            opening = None
        condition = html_block_start(content, paragraph is not None)
        thematic = THEMATIC_BREAK_RE.match(content)
        if atx:
            headings.append((start, len(atx.group(1)), ATX_CLOSING_RE.sub("", atx.group(2) or "").strip()))
            paragraph = None
        elif paragraph is not None and SETEXT_UNDERLINE_RE.match(content):
            # Link reference definitions leave the paragraph before it closes.
            lines, first = without_definitions(paragraph[0]), paragraph[2]
            paragraph = None
            if lines:
                headings.append((first, 1 if content[0] == "=" else 2, "\n".join(lines)))
            elif content[0] == "=":
                # With nothing left to close, the underline opens a paragraph.
                paragraph = ([plain], depth, start)
        elif opening:
            fence = (opening.group("marker")[0], len(opening.group("marker")), depth)
            paragraph = None
            masked.append(" " * len(line))
            continue
        elif condition:
            end = HTML_BLOCK_STARTS[condition - 1][1] if condition <= 5 else None
            html_end = (end, depth) if end is None or not end.search(expanded, col) else None
            paragraph = None
        elif thematic or FOOTNOTE_DEFINITION_RE.match(content):
            paragraph = None
            if not thematic:
                note_depth = depth
        elif (
            paragraph is not None
            and "|" in content
            and TABLE_DELIMITER_RE.match(content)
            and table_cells(content) == table_cells(paragraph[0][-1])
        ):
            # The last line of the paragraph becomes the header row, and the
            # table ends whatever is left of the paragraph.
            paragraph = None
            table_depth = depth
        elif paragraph is not None:
            paragraph[0].append(plain)
        else:
            paragraph = ([plain], depth, start)
        masked.append(line)

    return headings, "\n".join(masked)


def mask_code(text: str) -> str:
    """Mask fenced and inline code while preserving line numbers."""
    return INLINE_CODE_RE.sub(lambda m: " " * len(m.group(0)), scan_blocks(text)[1])


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


def html_heading_text(inner: str) -> str:
    """Reduce the inner HTML of a heading element to its text content."""
    # A line of only whitespace separates blocks and renders without it.
    inner = re.sub(r"(?m)^[ \t]+$", "", inner)
    return html.unescape(HTML_TAG_RE.sub("", HTML_COMMENT_RE.sub("", inner)))


def document_parts(text: str) -> tuple[str, list[tuple[int, int, str]], str]:
    """Return the document without front matter, its Markdown headings, and its body.

    The body is the document with fenced code blanked. A raw HTML heading or
    id inside fenced code does not exist for GitHub.
    """
    source = mask_front_matter(text)
    scanned, body = scan_blocks(source)
    return source, scanned, body


def visible_tags(body: str) -> tuple[str, list[tuple[int, int]]]:
    """Blank the HTML comments of ``body`` and list the spans of its inline code.

    Comments keep their length, so positions still line up.
    """
    tags = HTML_COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), body)
    return tags, [match.span() for match in INLINE_CODE_RE.finditer(body)]


def document_headings(text: str) -> list[tuple[int, int, str]]:
    """Return ``(offset, level, text)`` for every heading of the document.

    ATX and Setext headings come from the block scan and raw HTML headings
    from the remaining text. ``text`` is what GitHub derives an anchor from.
    """
    source, scanned, body = document_parts(text)
    tags, code_spans = visible_tags(body)
    headings = [(offset, level, heading_text(markup)) for offset, level, markup in scanned]
    for match in HTML_HEADING_RE.finditer(tags):
        if any(start < match.start() < end for start, end in code_spans):
            continue
        # An element left open ends at the next Markdown heading, which closes it.
        end = match.end(2)
        for start, _, _ in scanned:
            if match.start(2) < start < end:
                end = start
                break
        headings.append((match.start(), int(match.group(1)), html_heading_text(source[match.start(2) : end])))
    return sorted(headings)


def document_anchors(text: str) -> frozenset[str]:
    """Collect the explicit id anchors and heading anchors of a document."""
    _, _, body = document_parts(text)
    tags, code_spans = visible_tags(body)

    def in_code(position: int) -> bool:
        return any(start < position < end for start, end in code_spans)

    anchors: set[str] = set()
    for match in HTML_START_TAG_RE.finditer(tags):
        if in_code(match.start()):
            continue
        for attribute in ID_ATTRIBUTE_RE.finditer(match.group(2)):
            # GitHub keeps an id on any element, and a name on a link.
            if attribute.group(1).lower() == "id" or match.group(1).lower() == "a":
                value = next(group for group in attribute.groups()[1:] if group is not None)
                if value:
                    anchors.add(value)

    # GitHub suffixes the n-th repeat of a slug with "-n", starting at 1.
    occurrences: dict[str, int] = {}
    for _, _, heading in document_headings(text):
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

        # An underline or a heading indented to the content column of a list
        # item belongs to that item; other lines leave it.
        write(
            "docs/containers.md",
            "- foo",
            "  ---",
            "",
            "1. bar",
            "   ===",
            "",
            "-   baz",
            "    ---",
            "",
            "- > qux",
            "  > ===",
            "",
            "- alpha",
            "- ===",
            "",
            "- Outer",
            "  - Inner",
            "",
            "    ## 标题",
            "",
            "10. item",
            "    ## 宽列表里的标题",
            "",
            "> foo",
            ">",
            "> bar",
            "> ---",
            "",
            "- c",
            "- x",
            "  ---",
            "",
            "para",
            "- ",
            "  ---",
            "",
            "t",
            "- - -",
            "    ## h",
            "",
            "> quoted",
            "***",
            "> ---",
            "",
            "-   wide",
            "     ---",
            "",
            "-   narrow",
            "  ---",
            "",
            "> <div>",
            ">",
            "> # 标题",
        )

        # Lines inside an HTML block, a quoted fence, or a reopened fence are
        # not Markdown. A thematic break ends a paragraph, definitions and
        # footnotes do not turn into headings, and GitHub keeps an id on any
        # element.
        write(
            "docs/blocks.md",
            '<div align="center">',
            "## 不是标题",
            "</div>",
            "",
            "<!-- 注释",
            "# 也不是标题",
            "-->",
            "",
            "<details>",
            "段落",
            "---",
            "</details>",
            "",
            "> ```",
            "> # 围栏里的标题",
            "> ```",
            "",
            "## 引用围栏之后",
            "",
            "星号之前",
            "***",
            "---",
            "",
            "[foo]: /url",
            "====",
            "",
            "[^1]: note text",
            "---",
            "",
            "```#import <Foo>```",
            "",
            "## 行内三反引号之后",
            "",
            "```bash",
            "echo a",
            "```bash",
            "# 重开围栏里的标题",
            "```",
            "",
            "## 重开围栏之后",
            "",
            '<p id="custom_view">text</p>',
            '<span id="sp">text</span>',
            '<div id="dv"></div>',
            '<a name="named-anchor"></a>',
            "",
            "`<h4>代码里的标题</h4>`",
            "",
            '`<p id="in-code">x</p>`',
            "",
            '<!-- <p id="in-comment">x</p> -->',
            "",
            "text",
            "<x>",
            "---",
            "",
            "> c",
            "<x>",
            "# h",
            "",
            "</script>",
            "## closed",
            "",
            "[a]: /1",
            "next",
            "===",
            "",
            "[^1]: note",
            "more",
            "---",
            "",
            "| a |",
            "|---|",
            "foo",
            "===",
            "",
            "foo",
            "|---|---|",
            "---",
            "",
            "<h2>",
            "未闭合 HTML 标题",
            "",
            "## after-unclosed",
        )

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
            "- [列表项下划线](docs/containers.md#foo)",
            "- [有序列表下划线](docs/containers.md#bar)",
            "- [宽标记下划线](docs/containers.md#baz)",
            "- [列表里的引用下划线](docs/containers.md#qux)",
            "- [嵌套列表里的标题](docs/containers.md#标题)",
            "- [宽列表里的标题](docs/containers.md#宽列表里的标题)",
            "- [空引用行之后](docs/containers.md#bar-1)",
            "- [打断段落的列表项](docs/containers.md#x)",
            "- [空列表项不打断](docs/containers.md#para)",
            "- [宽标记五空格下划线](docs/containers.md#wide)",
            "- [引用里空标记之后的标题](docs/containers.md#标题-1)",
            "- [引用围栏之后](docs/blocks.md#引用围栏之后)",
            "- [行内三反引号之后](docs/blocks.md#行内三反引号之后)",
            "- [重开围栏之后](docs/blocks.md#重开围栏之后)",
            "- [p 的 id](docs/blocks.md#custom_view)",
            "- [span 的 id](docs/blocks.md#sp)",
            "- [div 的 id](docs/blocks.md#dv)",
            "- [a 的 name](docs/blocks.md#named-anchor)",
            "- [行内标签不打断](docs/blocks.md#text)",
            "- [引用定义之后的段落](docs/blocks.md#next)",
            "- [单元格数不匹配](docs/blocks.md#foo------)",
            "- [未闭合 HTML 标题](docs/blocks.md#未闭合-html-标题)",
            "- [同文件](#本地)",
            "- [无锚点](docs/flags.md)",
            "- [非 Markdown](data.txt#L10)",
            "- [外链](https://example.com/missing#anchor)",
            "- [引用式][ref]",
            "- [跨行引用式][ref-next]",
            "- 行内代码里的 `[fake](docs/flags.md#不存在)` 不算链接",
            "- 行内三反引号里的 ```[fake](docs/missing.md)``` 不算链接",
            "",
            "> ```",
            "> [fake](docs/missing.md) 不算链接",
            "> ```",
            "",
            "```bash",
            "echo a",
            "```bash",
            "[fake](docs/missing.md) 不算链接",
            "```",
            "",
            "[ref]: docs/flags.md#全角标点中文标题",
            "",
            "[ref-next]:",
            "  docs/flags.md#全角标点中文标题",
            "",
            "## 本地",
        )

        broken = [
            ("[跨文件](docs/flags.md#不存在的标题)", "missing link anchor: docs/flags.md#不存在的标题"),
            ("[同文件](#不存在的标题)", "missing link anchor: #不存在的标题"),
            ("[代码块](docs/flags.md#代码块里的标题)", "missing link anchor: docs/flags.md#代码块里的标题"),
            ("[大小写](docs/flags.md#Legacy)", "missing link anchor: docs/flags.md#Legacy"),
            ("[引用下的横线](docs/flags.md#引用里的段落)", "missing link anchor: docs/flags.md#引用里的段落"),
            ("[惰性续行](docs/flags.md#foo-bar)", "missing link anchor: docs/flags.md#foo-bar"),
            ("[惰性续行末行](docs/flags.md#bar)", "missing link anchor: docs/flags.md#bar"),
            ("[跨块拼接](docs/flags.md#普通段落-引用里的段落二)", "missing link anchor: docs/flags.md#普通段落-引用里的段落二"),
            ("[列表项](docs/flags.md#列表项不是标题)", "missing link anchor: docs/flags.md#列表项不是标题"),
            ("[元数据段落](docs/flags.md#name-flags)", "missing link anchor: docs/flags.md#name-flags"),
            ("[未闭合的元数据段落](docs/unterminated.md#name-unterminated)", "missing link anchor: docs/unterminated.md#name-unterminated"),
            ("[横线拼进标题](docs/thematic.md#----标题)", "missing link anchor: docs/thematic.md#----标题"),
            ("[代码里的 HTML 标题](docs/flags.md#代码里的标题)", "missing link anchor: docs/flags.md#代码里的标题"),
            ("[代码里的显式锚点](docs/flags.md#in-code)", "missing link anchor: docs/flags.md#in-code"),
            ("[HTML 重复错位](docs/flags.md#gnfs-2)", "missing link anchor: docs/flags.md#gnfs-2"),
            ("[HTML 块里的横线](docs/flags.md#qa-1)", "missing link anchor: docs/flags.md#qa-1"),
            ("[跨行 Setext 的连字符](docs/flags.md#跨行的-setext-标题)", "missing link anchor: docs/flags.md#跨行的-setext-标题"),
            ("[去掉缩进的 HTML 标题](docs/flags.md#多行-html-标题)", "missing link anchor: docs/flags.md#多行-html-标题"),
            ("[被改写的代码](docs/flags.md#使用-stdvector-与-x)", "missing link anchor: docs/flags.md#使用-stdvector-与-x"),
            ("[两个列表项](docs/containers.md#alpha)", "missing link anchor: docs/containers.md#alpha"),
            ("[跨空引用行](docs/containers.md#foo-bar)", "missing link anchor: docs/containers.md#foo-bar"),
            ("[分隔线后的缩进标题](docs/containers.md#h)", "missing link anchor: docs/containers.md#h"),
            ("[分隔线拼进引用](docs/containers.md#quoted)", "missing link anchor: docs/containers.md#quoted"),
            ("[列表项拼进段落](docs/containers.md#c---x)", "missing link anchor: docs/containers.md#c---x"),
            ("[窄缩进下划线](docs/containers.md#narrow)", "missing link anchor: docs/containers.md#narrow"),
            ("[HTML 块里的 ATX](docs/blocks.md#不是标题)", "missing link anchor: docs/blocks.md#不是标题"),
            ("[HTML 块里的段落](docs/blocks.md#段落)", "missing link anchor: docs/blocks.md#段落"),
            ("[注释里的 ATX](docs/blocks.md#也不是标题)", "missing link anchor: docs/blocks.md#也不是标题"),
            ("[引用围栏里的 ATX](docs/blocks.md#围栏里的标题)", "missing link anchor: docs/blocks.md#围栏里的标题"),
            ("[星号拼接](docs/blocks.md#星号之前)", "missing link anchor: docs/blocks.md#星号之前"),
            ("[引用定义之后](docs/blocks.md#foourl)", "missing link anchor: docs/blocks.md#foourl"),
            ("[脚注之后](docs/blocks.md#note-text)", "missing link anchor: docs/blocks.md#note-text"),
            ("[行内三反引号](docs/blocks.md#import-foo)", "missing link anchor: docs/blocks.md#import-foo"),
            ("[重开围栏里的 ATX](docs/blocks.md#重开围栏里的标题)", "missing link anchor: docs/blocks.md#重开围栏里的标题"),
            ("[代码里的 HTML 标题](docs/blocks.md#代码里的标题)", "missing link anchor: docs/blocks.md#代码里的标题"),
            ("[代码里的 id](docs/blocks.md#in-code)", "missing link anchor: docs/blocks.md#in-code"),
            ("[注释里的 id](docs/blocks.md#in-comment)", "missing link anchor: docs/blocks.md#in-comment"),
            ("[离开引用的标签块](docs/blocks.md#h)", "missing link anchor: docs/blocks.md#h"),
            ("[闭合标签块里的 ATX](docs/blocks.md#closed)", "missing link anchor: docs/blocks.md#closed"),
            ("[脚注续行](docs/blocks.md#more)", "missing link anchor: docs/blocks.md#more"),
            ("[表格后的下划线](docs/blocks.md#-a----foo)", "missing link anchor: docs/blocks.md#-a----foo"),
            ("[文件缺失](docs/missing.md#anchor)", "missing local link target: docs/missing.md#anchor"),
        ]
        # Reference definitions may put their destination on the next line.
        definitions = [
            ("[ref]: docs/flags.md#概述-3", "missing link anchor: docs/flags.md#概述-3"),
            ("[ref-next]:\n  docs/flags.md#概述-4", "missing link anchor: docs/flags.md#概述-4"),
        ]
        write(
            "broken.md",
            *[link for link, _ in broken],
            "",
            "[引用式][ref]",
            "",
            definitions[0][0],
            "",
            "[跨行引用式][ref-next]",
            "",
            definitions[1][0],
        )
        expected = [f"broken.md:{number}: {message}" for number, (_, message) in enumerate(broken, start=1)]
        expected += [
            f"broken.md:{len(broken) + 4}: {definitions[0][1]}",
            f"broken.md:{len(broken) + 8}: {definitions[1][1]}",
        ]
        failures = scan(root, [root / "valid.md", root / "broken.md"])
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
