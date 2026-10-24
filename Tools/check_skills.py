"""
check_skills.py -- the skills in Resources/Skills follow the Agent Skills format that every client
reads (Claude Code, Codex, Cursor, VS Code, Antigravity CLI, Gemini CLI), and name only tools,
parameters and answer fields that the catalog has.

Usage:
  python Tools/check_skills.py

The editor's MCP menu copies these folders into a project (.claude/skills, .agents/skills) and stamps
each SKILL.md with the plugin version under `metadata`, so a shipped SKILL.md carries no stamp of its
own. A skill ships with the plugin so that it names the tools of that version: a name the catalog
does not know fails here, before a client follows it to a tool that is not there.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from _paths import CATALOG_JSON_P, PLUGIN_ROOT_P, SOURCE_DIR_P

SKILLS_DIR = PLUGIN_ROOT_P / "Resources" / "Skills"
PREFIX = "ueagent-"
# The Agent Skills specification's fields (agentskills.io/specification). A client-specific field
# would read differently in the other clients.
ALLOWED_FIELDS = {"name", "description", "license", "compatibility", "metadata", "allowed-tools"}
NAME_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
# A snake_case word in backticks: a tool, a parameter or an answer field.
CODE_WORD_RE = re.compile(r"`([a-z][a-z0-9]*(?:_[a-z0-9]+)+)`")
# Placeholders and glob patterns in backticks (`get_*`) are not names.
LINK_RE = re.compile(r"\]\(([^)\s]+)\)")
CLIENT_ONLY = ("$ARGUMENTS", "argument-hint:")


def server_words() -> set[str]:
    """What the server itself adds, outside the catalog: the compact list's meta-tools (find_tools,
    describe_tool, call_tool) and the fields of the reply envelope (unknown_params...). Read from the
    string literals of the server's sources, so a rename there is seen here."""
    words: set[str] = set()
    for source in (SOURCE_DIR_P / "Private").glob("*.cpp"):
        words.update(re.findall(r'TEXT\("([a-z][a-z0-9]*(?:_[a-z0-9]+)+)"\)', source.read_text(encoding="utf-8", errors="replace")))
    for source in (SOURCE_DIR_P / "Private" / "Mcp").rglob("*.cpp"):
        words.update(re.findall(r'TEXT\("([a-z][a-z0-9]*(?:_[a-z0-9]+)+)"\)', source.read_text(encoding="utf-8", errors="replace")))
    return words


def catalog_words() -> tuple[set[str], set[str]]:
    """The listed tool names, and every word the catalog and the server use (parameters, answer fields, descriptions,
    pack names).

    A retired tool or a renamed tool's old name still runs, but a skill names what tools/list shows."""
    catalog = json.loads(CATALOG_JSON_P.read_text(encoding="utf-8"))
    tools: set[str] = set()
    words: set[str] = server_words()
    words.update(pack["name"] for pack in catalog.get("packs") or [])
    for domain in catalog["domains"]:
        for command in domain["commands"]:
            if not command.get("retired"):
                tools.add(command["name"])
            words.update(p["name"] for p in command.get("params", []))
            for text in [command.get("description", ""), command.get("returns", "")] + [
                p.get("description", "") for p in command.get("params", [])
            ]:
                words.update(re.findall(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+", text or ""))
    return tools, words


def frontmatter(text: str) -> tuple[dict[str, str], list[str], str] | None:
    """The top-level fields (a map's own lines are kept as its value), the field order, and the body."""
    lines = text.split("\n")
    if not lines or lines[0].rstrip() != "---":
        return None
    try:
        close = next(i for i in range(1, len(lines)) if lines[i].rstrip() == "---")
    except StopIteration:
        return None
    fields: dict[str, str] = {}
    order: list[str] = []
    current = None
    for line in lines[1:close]:
        if line[:1] in (" ", "\t") and current:
            fields[current] += "\n" + line
            continue
        key, _, value = line.partition(":")
        current = key.strip()
        fields[current] = value.strip()
        order.append(current)
    return fields, order, "\n".join(lines[close + 1:])


def check_skill(folder: Path, tools: set[str], words: set[str]) -> list[str]:
    problems: list[str] = []
    rel = folder.relative_to(PLUGIN_ROOT_P).as_posix()
    skill_md = folder / "SKILL.md"
    if not skill_md.is_file():
        return [f"{rel}: no SKILL.md"]
    raw = skill_md.read_bytes()
    if b"\r\n" in raw:
        problems.append(f"{rel}/SKILL.md: CRLF line endings (the repository uses LF)")
    text = raw.decode("utf-8").replace("\r\n", "\n")
    parsed = frontmatter(text)
    if parsed is None:
        return problems + [f"{rel}/SKILL.md: no frontmatter between two '---' lines at the top"]
    fields, order, body = parsed

    for key in order:
        if key not in ALLOWED_FIELDS:
            problems.append(f"{rel}/SKILL.md: field '{key}' is not in the Agent Skills format; other clients would not read it the same way")
    name = fields.get("name", "")
    if not NAME_RE.match(name) or len(name) > 64:
        problems.append(f"{rel}/SKILL.md: name '{name}' must be lowercase letters, digits and single hyphens, at most 64 characters")
    if name != folder.name:
        problems.append(f"{rel}/SKILL.md: name '{name}' must equal its folder's name, '{folder.name}'")
    if not name.startswith(PREFIX):
        problems.append(f"{rel}/SKILL.md: name '{name}' must start with '{PREFIX}': the installer only ever replaces or deletes folders with that prefix")
    description = fields.get("description", "")
    if not 1 <= len(description) <= 1024:
        problems.append(f"{rel}/SKILL.md: description has {len(description)} characters (1 to 1024)")
    if len(fields.get("compatibility", "")) > 500:
        problems.append(f"{rel}/SKILL.md: compatibility is over 500 characters")
    if "ueagent-version" in fields.get("metadata", ""):
        problems.append(f"{rel}/SKILL.md: carries a ueagent-version stamp; the installer adds it, from the plugin's version")

    for marker in CLIENT_ONLY:
        if marker in text:
            problems.append(f"{rel}/SKILL.md: '{marker}' is a Claude Code feature; the other clients would show it as is")

    for doc in sorted(folder.rglob("*.md")):
        doc_text = doc.read_text(encoding="utf-8")
        for word in sorted(set(CODE_WORD_RE.findall(doc_text))):
            if word not in tools and word not in words:
                problems.append(f"{doc.relative_to(PLUGIN_ROOT_P).as_posix()}: `{word}` is no tool, parameter or answer field in the catalog")
        for target in LINK_RE.findall(doc_text):
            if "://" in target or target.startswith(("#", "mailto:")):
                continue
            if not (doc.parent / target.split("#")[0]).exists():
                problems.append(f"{doc.relative_to(PLUGIN_ROOT_P).as_posix()}: link to '{target}', which does not exist")
    if len(body.split("\n")) > 500:
        problems.append(f"{rel}/SKILL.md: over 500 lines; move the reference material to a file next to it")
    return problems


def main() -> int:
    if not SKILLS_DIR.is_dir():
        print(f"FAIL: {SKILLS_DIR} does not exist")
        return 1
    tools, words = catalog_words()
    folders = sorted(p for p in SKILLS_DIR.iterdir() if p.is_dir())
    problems = [p for folder in folders for p in check_skill(folder, tools, words)]
    if not folders:
        problems.append(f"{SKILLS_DIR}: no skills")
    for problem in problems:
        print(f"FAIL: {problem}")
    if problems:
        return 1
    print(f"OK: {len(folders)} skills follow the Agent Skills format and name only what the catalog has")
    return 0


if __name__ == "__main__":
    sys.exit(main())
