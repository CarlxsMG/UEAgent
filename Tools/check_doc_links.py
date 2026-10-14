"""
check_doc_links.py -- every relative link in the repository's Markdown points at a file that exists,
and every #anchor at a heading of the file it names.

Usage:
  python Tools/check_doc_links.py

Covers Markdown links and images, and the href/src of inline HTML. Links to the web (http, https,
mailto) are not followed: that would make the check depend on the network.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_DIRS = {".git", "Binaries", "Intermediate", "Saved", "DerivedDataCache"}

MD_LINK_RE = re.compile(r"!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_LINK_RE = re.compile(r"\b(?:href|src)=\"([^\"]+)\"")
HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"`[^`]*`")


def markdown_files() -> list[Path]:
    return sorted(p for p in ROOT.rglob("*.md") if not SKIP_DIRS.intersection(p.relative_to(ROOT).parts))


def prose_lines(path: Path) -> list[str]:
    """The file's lines outside fenced code blocks, where a `#` or a link is not markup."""
    lines, in_fence = [], False
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append(line)
    return lines


def exists_exactly(path: Path) -> bool:
    """Exists with this exact letter case: Windows accepts any case, the CI's Linux does not."""
    if not path.exists():
        return False
    try:
        parts = path.relative_to(ROOT).parts
    except ValueError:
        return True  # outside the repository: nothing to compare
    folder = ROOT
    for part in parts:
        if part not in os.listdir(folder):
            return False
        folder = folder / part
    return True


def slug(heading: str) -> str:
    """GitHub's anchor for a heading: lower case, punctuation dropped, spaces as hyphens."""
    text = heading.replace("`", "").strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def anchors(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        found: set[str] = set()
        seen: dict[str, int] = {}
        for line in prose_lines(path):
            match = HEADING_RE.match(line)
            if not match:
                continue
            base = slug(match.group(1))
            count = seen.get(base, 0)
            seen[base] = count + 1
            found.add(base if count == 0 else f"{base}-{count}")
        cache[path] = found
    return cache[path]


def main() -> int:
    cache: dict[Path, set[str]] = {}
    broken: list[str] = []
    files = markdown_files()
    links = 0
    for md in files:
        for line in prose_lines(md):
            text = INLINE_CODE_RE.sub("", line)
            for target in MD_LINK_RE.findall(text) + HTML_LINK_RE.findall(text):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE):
                    continue  # http:, https:, mailto: ...
                links += 1
                path_part, _, anchor = target.partition("#")
                # normpath, not resolve(): resolve() would correct the letter case on Windows.
                resolved = Path(os.path.normpath(md.parent / path_part)) if path_part else md
                where = f"{md.relative_to(ROOT).as_posix()}: {target}"
                if not exists_exactly(resolved):
                    broken.append(f"{where} -- no such file (letter case included)")
                elif anchor and resolved.suffix == ".md" and anchor.lower() not in anchors(resolved, cache):
                    broken.append(f"{where} -- no heading for #{anchor}")
    print(f"{len(files)} Markdown files, {links} relative links")
    for line in broken:
        print(f"  BROKEN {line}")
    if broken:
        print(f"FAIL: {len(broken)} broken links")
        return 1
    print("  every relative link resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
