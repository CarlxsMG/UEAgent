"""
mkdocs_hooks.py -- what the documentation site adds to docs/ when MkDocs builds it (`hooks:` in mkdocs.yml).

The Markdown in docs/ stays readable on GitHub; the site gets, at build time:

- includes: a line `<!-- include: README.md#settings -->` becomes that section of a repository file (the whole file
  without `#section`), with its relative links pointed at the matching site page, or at GitHub. What follows it up
  to a line `<!-- /include -->` is what GitHub shows instead, and the site drops it;
- tabs: the headings between `<!-- tabs -->` and `<!-- /tabs -->` become content tabs;
- tool links: an inline code span that is exactly a tool's name links to that tool in the reference;
- chips: on a tool page, each tool's annotations ("*Read-only.*") as colored labels, plus one on the tools that
  Play-in-Editor refuses;
- a description for every page that has none in its front matter: its first paragraph;
- table cells: `\\|` inside a code span reads as `|`, as on GitHub (Python-Markdown keeps the backslash there);
- the tool reference's domains in the navigation, read from the catalog;
- after the build: assets/catalog/tools.json (the tool explorer's data), assets/social-preview.png (the image a
  shared link shows) and llms.txt (the site, for agents).
"""
from __future__ import annotations

import json
import posixpath
import re
import shutil
import sys
from pathlib import Path

from mkdocs.exceptions import PluginError

sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalog_server  # noqa: E402
from check_doc_links import slug  # noqa: E402
from codegen_tool_docs import summary  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SOCIAL_IMAGE = ROOT / ".github" / "social-preview.png"

INCLUDE_RE = re.compile(r"^<!-- include: (?P<path>[^#\s]+)(?:#(?P<section>[\w-]+))? -->[ \t]*$"
                        r"(?:\n(?:(?!<!-- include:).)*?^<!-- /include -->[ \t]*$)?", re.M | re.S)
TABS_RE = re.compile(r"^<!-- tabs -->\n(?P<body>.*?)^<!-- /tabs -->[ \t]*$", re.M | re.S)
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
MD_TARGET_RE = re.compile(r"(\]\(\s*)([^)\s]+)")
HTML_TARGET_RE = re.compile(r"(\b(?:href|src)=\")([^\"]+)")
INLINE_CODE_RE = re.compile(r"(`+).+?\1")
LINK_RE = re.compile(r"!?\[[^\]]*\]\([^)]*\)")
TOOL_SPAN_RE = re.compile(r"`([a-z][a-z0-9_]*)`")
TOOL_HINT_RE = re.compile(r"^## `(?P<name>[a-z0-9_]+)`\n\n(?:\*(?P<hint>[A-Z][^*\n]*)\.\*\n\n)?", re.M)

CHIP_TITLES = {
    "read-only": "Reads the editor and changes nothing",
    "additive": "Adds to the project without changing or removing what is there",
    "destructive": "Can change or remove what is already in the project",
    "idempotent": "Calling it again with the same arguments changes nothing more",
    "open-world": "Talks to the network or runs arbitrary code",
    "pie": "Refused while Play-in-Editor runs, unless the call passes force: true",
}

TOOLS: dict[str, str] = {}      # listed tool name -> its domain
GONE: set[str] = set()          # retired tools and old names: they link to the Retired names page
DOMAINS: list[dict] = []        # the catalog's domains with their listed tools, sorted by name
PIE_REFUSED: set[str] = set()   # the tools the PIE guard refuses while Play-in-Editor runs
NAV = None


# ------------------------------------------------------------------ shared helpers
def outside_fences(lines: list[str]):
    """Yields (index, line) for the lines outside fenced code blocks."""
    in_fence = False
    for index, line in enumerate(lines):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            yield index, line


def outside_code(line: str, change) -> str:
    """The line with `change` applied to its text outside inline code spans."""
    out, at = [], 0
    for match in INLINE_CODE_RE.finditer(line):
        out += [change(line[at:match.start()]), match.group(0)]
        at = match.end()
    out.append(change(line[at:]))
    return "".join(out)


# ------------------------------------------------------------------ includes
def section(text: str, anchor: str, source: str) -> str:
    """The body of the section under the heading whose anchor is `anchor`, up to the next heading of its level."""
    lines = text.splitlines()
    start = level = None
    for index, line in outside_fences(lines):
        match = HEADING_RE.match(line)
        if not match:
            continue
        if start is None and slug(match.group(2)) == anchor:
            start, level = index + 1, len(match.group(1))
        elif start is not None and len(match.group(1)) <= level:
            return "\n".join(lines[start:index])
    if start is None:
        raise PluginError(f"include: {source} has no heading for #{anchor}")
    return "\n".join(lines[start:])


def retarget(target: str, source: str, page: str, repo_url: str) -> str:
    """A relative link of an included file, made to work from the page that includes it."""
    if target.startswith("#") or re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE):
        return target
    path, _, anchor = target.partition("#")
    repo_path = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
    if repo_path.startswith("docs/") and (ROOT / repo_path).is_file():
        new = posixpath.relpath(repo_path[len("docs/"):], posixpath.dirname(page) or ".")
    else:
        new = f"{repo_url}/{'tree' if (ROOT / repo_path).is_dir() else 'blob'}/main/{repo_path}"
    return new + (f"#{anchor}" if anchor else "")


def expand_includes(markdown: str, page: str, repo_url: str) -> str:
    def include(match: re.Match) -> str:
        source = match["path"]
        path = ROOT / source
        if not path.is_file():
            raise PluginError(f"include: no such file {source} (in docs/{page})")
        text = path.read_text(encoding="utf-8")
        if match["section"]:
            text = section(text, match["section"], source)
        lines = text.strip("\n").splitlines()

        def fix(chunk: str) -> str:
            def swap(m: re.Match) -> str:
                return m.group(1) + retarget(m.group(2), source, page, repo_url)
            return HTML_TARGET_RE.sub(swap, MD_TARGET_RE.sub(swap, chunk))

        for index, line in outside_fences(lines):
            lines[index] = outside_code(line, fix)
        return "\n".join(lines)

    return INCLUDE_RE.sub(include, markdown)


# ------------------------------------------------------------------ tabs
def make_tabs(markdown: str) -> str:
    def tabs(match: re.Match) -> str:
        lines = match["body"].splitlines()
        headings = [(i, HEADING_RE.match(line)) for i, line in outside_fences(lines)]
        headings = [(i, m) for i, m in headings if m]
        if not headings:
            return match["body"]
        level = len(headings[0][1].group(1))
        starts = {i: m.group(2) for i, m in headings if len(m.group(1)) == level}
        out = lines[:headings[0][0]]
        for index, line in enumerate(lines[headings[0][0]:], start=headings[0][0]):
            if index in starts:
                out += ["", f'=== "{starts[index]}"', ""]
            else:
                out.append(f"    {line}" if line.strip() else "")
        return "\n".join(out) + "\n"

    return TABS_RE.sub(tabs, markdown)


# ------------------------------------------------------------------ tool pages and tool links
def chips(markdown: str) -> str:
    def label(match: re.Match) -> str:
        words = [w.strip() for w in match["hint"].split(",")] if match["hint"] else []
        kinds = [(w.lower().replace(" ", "-"), w[:1].upper() + w[1:]) for w in words]
        if match["name"] in PIE_REFUSED:
            kinds.append(("pie", "Refused during PIE"))
        if not kinds:
            return match.group(0)
        spans = " ".join(f'<span class="ua-chip ua-chip--{kind}" title="{CHIP_TITLES.get(kind, "")}">{text}</span>'
                         for kind, text in kinds)
        return f"## `{match['name']}`\n\n<p class=\"ua-chips\">{spans}</p>\n\n"

    return TOOL_HINT_RE.sub(label, markdown)


def tool_href(name: str, page: str) -> str:
    if name not in TOOLS:
        return posixpath.relpath("tools/retired.md", posixpath.dirname(page) or ".")
    target = f"tools/{TOOLS[name]}.md"
    if target == page:
        return f"#{name}"
    return posixpath.relpath(target, posixpath.dirname(page) or ".") + f"#{name}"


def link_tools(markdown: str, page: str) -> str:
    """Every inline code span that is exactly a tool's name, outside links and headings, made a link to the tool."""
    def link(match: re.Match) -> str:
        name = match.group(1)
        return f"[`{name}`]({tool_href(name, page)})" if name in TOOLS or name in GONE else match.group(0)

    def in_text(text: str) -> str:
        out, at = [], 0
        for existing in LINK_RE.finditer(text):
            out += [TOOL_SPAN_RE.sub(link, text[at:existing.start()]), existing.group(0)]
            at = existing.end()
        out.append(TOOL_SPAN_RE.sub(link, text[at:]))
        return "".join(out)

    tool_page = page.startswith("tools/")
    lines = markdown.splitlines()
    for index, line in outside_fences(lines):
        if HEADING_RE.match(line) or "`" not in line:
            continue
        if tool_page and line.startswith("| `"):
            # A parameter table: its first cell is a parameter, and two parameters share a tool's name.
            first, _, rest = line[2:].partition(" | ")
            lines[index] = f"| {first} | {in_text(rest)}"
        else:
            lines[index] = in_text(line)
    return "\n".join(lines) + ("\n" if markdown.endswith("\n") else "")


# ------------------------------------------------------------------ descriptions
def first_paragraph(markdown: str) -> str:
    """The page's first paragraph of prose, as plain text, for its description."""
    paragraph: list[str] = []
    for _, line in outside_fences(markdown.splitlines()):
        text = line.strip()
        if not text:
            if paragraph:
                break
            continue
        block = text.startswith(("#", "<", "|", "!", "- ", "* ", "=", ":", ">")) or re.match(r"^\d+\. ", text)
        if block or (not paragraph and text.startswith("[")):
            if paragraph:
                break
            continue
        paragraph.append(text)
    text = " ".join(paragraph)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\{[^}]*\}|<[^>]+>|\*\*|__|`", "", text).replace("*", "")
    if len(text) > 200:
        text = text[:197].rsplit(" ", 1)[0].rstrip(",;:") + "..."
    return text


# ------------------------------------------------------------------ table cells
CELL_RE = re.compile(r"(<t[dh][^>]*>)(.*?)(</t[dh]>)", re.S)
CODE_RE = re.compile(r"(<code[^>]*>)(.*?)(</code>)", re.S)


def unescape_cell_pipes(html: str) -> str:
    """A table cell's code spans with `\\|` read as `|`. In a table the Markdown has to escape a pipe even inside a
    code span; GitHub drops the backslash there, Python-Markdown shows it. Outside tables the HTML is left alone."""
    def code(match: re.Match) -> str:
        return match.group(1) + match.group(2).replace("\\|", "|") + match.group(3)

    def cell(match: re.Match) -> str:
        return match.group(1) + CODE_RE.sub(code, match.group(2)) + match.group(3)

    return CELL_RE.sub(cell, html)


# ------------------------------------------------------------------ MkDocs events
def on_config(config):
    with open(catalog_server.CATALOG, encoding="utf-8") as f:
        catalog = json.load(f)
    # Retired tools still run but are not listed, so they have no entry in the reference: their names, and the old
    # names of renamed tools, link to the Retired names page instead.
    DOMAINS[:] = sorted(({"domain": d["domain"], "description": d.get("description", ""),
                          "commands": [c for c in d["commands"] if not c.get("retired")]}
                         for d in catalog["domains"]), key=lambda d: d["domain"])
    DOMAINS[:] = [d for d in DOMAINS if d["commands"]]
    TOOLS.clear()
    TOOLS.update({c["name"]: d["domain"] for d in DOMAINS for c in d["commands"]})
    GONE.clear()
    GONE.update(c["name"] for d in catalog["domains"] for c in d["commands"] if c.get("retired"))
    GONE.update(catalog.get("renamed") or {})
    PIE_REFUSED.clear()
    PIE_REFUSED.update(catalog_server.pie_unsafe_commands(catalog_server.PIE_TABLE.read_text(encoding="utf-8")))

    # The entry `Tool reference: tools/README.md` becomes a section: the index, then one page per domain.
    def expand(items: list) -> bool:
        for position, item in enumerate(items):
            if not isinstance(item, dict):
                continue
            (title, value), = item.items()
            if value == "tools/README.md":
                items[position] = {title: ["tools/README.md"] + [{d["domain"]: f"tools/{d['domain']}.md"}
                                                                   for d in DOMAINS]
                                   + ([{"Retired names": "tools/retired.md"}] if GONE else [])}
                return True
            if isinstance(value, list) and expand(value):
                return True
        return False

    if not expand(config["nav"] or []):
        raise PluginError("mkdocs.yml: the nav has no entry for tools/README.md")
    return config


def on_page_markdown(markdown, page, config, files):
    source = page.file.src_uri
    repo_url = config["repo_url"].rstrip("/")
    whole = INCLUDE_RE.fullmatch(markdown.strip())
    if whole and not whole["section"]:
        # The page is one repository file: "Edit this page" edits that file, not the page that includes it.
        page.edit_url = f"{repo_url}/edit/main/{whole['path']}"
    markdown = expand_includes(markdown, source, repo_url)
    if not page.meta.get("description"):
        page.meta["description"] = first_paragraph(markdown) or config["site_description"]
    markdown = make_tabs(markdown)
    if source.startswith("tools/") and source != "tools/README.md":
        markdown = chips(markdown)
    return link_tools(markdown, source)


def on_page_content(html, page, config, files):
    return unescape_cell_pipes(html)


def on_nav(nav, config, files):
    global NAV
    NAV = nav
    return nav


def on_post_build(config):
    site = Path(config["site_dir"])
    assets = site / "assets"
    (assets / "catalog").mkdir(parents=True, exist_ok=True)

    tools = []
    for index, domain in enumerate(DOMAINS):
        for command in sorted(domain["commands"], key=lambda c: c["name"]):
            hints = command.get("annotations") or {}
            flags = "r" if hints.get("readOnlyHint") else ("d" if hints.get("destructiveHint", True) else "a")
            flags += "i" if hints.get("idempotentHint") else ""
            flags += "o" if hints.get("openWorldHint") else ""
            flags += "p" if command["name"] in PIE_REFUSED else ""
            params = [[p["name"], p.get("jsonType") or "any", 1 if p.get("required") else 0, p.get("description") or ""]
                      for p in command.get("params") or []]
            tools.append([command["name"], index, flags, command.get("description") or "", params,
                          command.get("returns") or ""])
    catalog = {"domains": [d["domain"] for d in DOMAINS],
               "summaries": [summary(d) for d in DOMAINS],
               "tools": tools}
    with open(assets / "catalog" / "tools.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(catalog, f, ensure_ascii=False, separators=(",", ":"))

    shutil.copyfile(SOCIAL_IMAGE, assets / "social-preview.png")
    (site / "llms.txt").write_text(llms_txt(config), encoding="utf-8", newline="\n")


def llms_txt(config) -> str:
    """The site as llmstxt.org describes it: a summary, then a list of links per section."""
    base = config["site_url"]
    lines = [f"# {config['site_name']}", "", f"> {config['site_description']}", "",
             "UEAgent is an editor plugin for Unreal Engine 5.7 and 5.8. While the editor is open it serves MCP over Streamable "
             "HTTP at http://127.0.0.1:55561/mcp, and every tool call runs on the editor's game thread. By default it "
             "lists its core tools plus find_tools, describe_tool, call_tool and enable_toolset, which reach the rest; "
             "?tools=all lists every tool and ?tools=compact only the first three.", ""]

    def entry(page) -> str:
        description = page.meta.get("description", "")
        return f"- [{page.title}]({base}{page.url})" + (f": {description}" if description else "")

    def walk(items, title: str) -> None:
        pages = [item for item in items if item.is_page]
        if pages:
            lines.extend([f"## {title}", ""] + [entry(p) for p in pages] + [""])
        for item in items:
            if item.is_section:
                walk(item.children, item.title)

    walk(NAV.items if NAV else [], "Overview")
    lines += ["## Data", "",
              f"- [Tool catalog]({base}assets/catalog/tools.json): every tool as JSON, by domain, with its "
              "description, parameters, return shape and flags (r read-only, a additive, d destructive, "
              "i idempotent, o open world, p refused during Play-in-Editor)", ""]
    return "\n".join(lines)
