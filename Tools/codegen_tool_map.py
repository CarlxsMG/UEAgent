"""
codegen_tool_map.py -- the generated parts of the ueagent-tool-map skill (Resources/Skills/ueagent-tool-map/SKILL.md):
the core tools, the packs and the domains, from Resources/command_catalog.json.

The skill is also the server's ueagent://guide resource and its ueagent-tool-map prompt, so an agent reads the same map
whichever way its client reaches it. Its prose is written by hand; the blocks between
`<!-- BEGIN codegen_tool_map: <block> ... -->` and `<!-- END codegen_tool_map: <block> -->` are rewritten from the
catalog, so a tool moved to another pack, or a new domain, reaches the map with the next --write. Run it after
codegen_command_catalog.py.

Usage:
  python Tools/codegen_tool_map.py            # report whether the skill would change
  python Tools/codegen_tool_map.py --write    # rewrite the generated blocks
  python Tools/codegen_tool_map.py --check    # fail if the skill drifted from the catalog
"""
from __future__ import annotations

import argparse
import json
import re
import sys

from _paths import CATALOG_JSON_P, PLUGIN_ROOT_P

SKILL_MD = PLUGIN_ROOT_P / "Resources" / "Skills" / "ueagent-tool-map" / "SKILL.md"
BLOCKS = ("core", "packs", "domains")
BLOCK_RE = re.compile(
    r"(<!-- BEGIN codegen_tool_map: (?P<name>[a-z]+)[^\n]*-->\n)(?P<body>.*?)(<!-- END codegen_tool_map: (?P=name) -->)",
    re.S)


def capital(text: str) -> str:
    return text[:1].upper() + text[1:]


def names(tools: list[str]) -> str:
    return ", ".join(f"`{name}`" for name in tools)


def render(catalog: dict) -> dict[str, str]:
    """Each block's lines, from the catalog."""
    listed = [(d, c) for d in catalog["domains"] for c in d["commands"] if not c.get("retired")]
    core = sorted(c["name"] for _, c in listed if c.get("core"))
    blocks = {"core": f"The core tools: {names(core)}.\n"}

    lines = ["| Pack | For | Tools |", "|---|---|---|"]
    for pack in catalog.get("packs") or []:
        tools = sorted(c["name"] for _, c in listed if c.get("pack") == pack["name"])
        lines.append(f"| `{pack['name']}` | {capital(pack['description'])} | {names(tools)} |")
    blocks["packs"] = "\n".join(lines) + "\n"

    lines = ["| Domain | Tools | What it covers |", "|---|---|---|"]
    for domain in sorted(catalog["domains"], key=lambda d: d["domain"]):
        count = sum(1 for c in domain["commands"] if not c.get("retired"))
        if count:
            lines.append(f"| {domain['domain']} | {count} | {capital(domain.get('description') or '')} |")
    blocks["domains"] = "\n".join(lines) + "\n"
    return blocks


def rewrite(text: str, blocks: dict[str, str]) -> tuple[str, list[str]]:
    """The skill with its blocks replaced, and the blocks it lacks."""
    found: list[str] = []

    def replace(m: re.Match) -> str:
        found.append(m.group("name"))
        return m.group(1) + blocks.get(m.group("name"), m.group("body")) + m.group(4)

    out = BLOCK_RE.sub(replace, text)
    return out, [name for name in BLOCKS if name not in found]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--write", action="store_true", help="rewrite the generated blocks of the skill")
    p.add_argument("--check", action="store_true", help="fail if the skill drifted from the catalog")
    args = p.parse_args()

    catalog = json.loads(CATALOG_JSON_P.read_text(encoding="utf-8"))
    have = SKILL_MD.read_text(encoding="utf-8")
    want, missing = rewrite(have, render(catalog))
    if missing:
        print(f"FAIL: {SKILL_MD.relative_to(PLUGIN_ROOT_P).as_posix()} has no block {', '.join(missing)}")
        return 1
    changed = want != have
    print(f"{SKILL_MD.relative_to(PLUGIN_ROOT_P).as_posix()}: {'changed' if changed else 'in sync with the catalog'}")
    if args.check:
        if changed:
            print("TOOL MAP DRIFT: run Tools/codegen_tool_map.py --write")
            return 1
        return 0
    if args.write and changed:
        SKILL_MD.write_text(want, encoding="utf-8", newline="\n")
        print(f"WROTE   : {SKILL_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
