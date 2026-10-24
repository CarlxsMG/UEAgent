"""
codegen_command_catalog.py — single source of truth regenerator.

Generates `Resources/command_catalog.json` directly from the plugin's C++ so
catalog and plugin can never drift. The source is each command family's .cpp:
its route table (the McpCommandFamilies::FRegistrar the bridge dispatches
through, one `{ TEXT("xxx"), &HandleXxxYyy }` row per command) names every
command string the family answers and the function that runs it, and that
function's `Params->GetStringField("yyy")` reads give the param shape.

The script:
  * parses every family,
  * fails on a command string that two families register (the registry keeps one),
  * flags naming drift (snake_case that doesn't pass basic heuristics),
  * maps every family to a domain,
  * emits `command_catalog.json` from the cpp ONLY (params + descriptions live
    in `// MCP:` annotations; there is no on-disk merge) when --write is on.

Usage:
  python Tools/codegen_command_catalog.py            # report only (default)
  python Tools/codegen_command_catalog.py --write    # regenerate the JSON
  python Tools/codegen_command_catalog.py --check    # fail if the catalog differs from the C++ in anything
  python Tools/codegen_command_catalog.py --strict   # fail on naming drift
"""

from __future__ import annotations
import argparse
import json
import os
import re
import sys
import glob
from collections import defaultdict, OrderedDict
from functools import lru_cache

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _paths  # noqa: E402
import tool_hints  # noqa: E402

HANDLER_GLOB = os.path.join(_paths.COMMANDS_DIR, "**", "*.cpp")
# The plugin's MCP server (Mcp/McpToolCatalog.cpp) reads this file at startup.
OUT_JSON = _paths.CATALOG_JSON
DIFF_TXT = _paths.report_path("command_catalog_diff.txt")

# The meta-tools of the compact tool list (Mcp/McpMetaTools.cpp): the server answers these
# names itself, so a command registered under one would be unreachable.
META_TOOL_NAMES = {"find_tools", "describe_tool", "call_tool", "enable_toolset"}

# One verb per operation (the server's instructions and CONTRIBUTING.md name them): a tool name that starts with a
# synonym of a verb the catalog uses fails the build, so the synonym cannot come back. Value: the verb to use.
SYNONYM_VERBS = {
    "read": "get", "inspect": "get", "fetch": "get", "retrieve": "get", "dump": "list",
    "query": "find", "search": "find", "lookup": "find", "detect": "is", "compare": "diff",
    "configure": "set", "edit": "set", "update": "set", "modify": "set", "change": "set",
    "make": "create", "scaffold": "create", "setup": "add", "append": "add", "insert": "add",
    "place": "spawn", "destroy": "delete", "erase": "delete",
}
# Names kept on purpose, each with its reason.
SYNONYM_VERB_EXCEPTIONS = {
    "edit_level_instance": "the engine's edit mode of a level instance, which commit_level_instance leaves",
    "insert_usd_sublayer": "USD's own operation: a sublayer goes in at a position of the layer stack",
}

# Skip these — utilities, not command handlers.
SKIP_GROUPS = {"Common"}

# Handler .cpp parsing. A family's route table sends each command to a separate
# `HandleXxx(Params)` function, and the params are read INSIDE that function. So we
# resolve `cmd -> HandleXxx` from the table and scan that function's brace-balanced body.
#   { TEXT("cmd"), &HandleXxx },
ROUTE_ENTRY_RE = re.compile(r'\{\s*TEXT\("([^"]+)"\)\s*,\s*&\s*(?:\w+::)?(Handle\w+)\s*\}')

# Param reads inside a handler body. Matches BOTH the return-style `Get*Field`
# and the out-param-style `TryGet*Field`. The read MUST be on the `Params`
# receiver (`Params->Get*Field(TEXT("name"))`): reads off a RESULT object
# (`CineRes->GetBoolField(TEXT("success"))`) or a nested sub-object
# (`ColorObj->GetNumberField(TEXT("r"))`, `J->GetNumberField(TEXT("time"))`)
# are NOT input params — anchoring to `Params` keeps those out of the catalog.
# Helpers that forward `Params` name their parameter `Params` too (e.g.
# ReadPosition(const TSharedPtr<FJsonObject>& Params)), so following them
# (see _collect_params) still matches.
FIELD_RE = re.compile(
    r'\bParams\s*->\s*(?:Try)?Get(String|Number|Integer|Bool|Array|Object|StringArray)Field'
    r'\s*\(\s*TEXT\("([^"]+)"\)'
)
# `Params->(Try)?GetField(TEXT("x"))` — the RAW/untyped getter, returning a
# `TSharedPtr<FJsonValue>` the handler forwards as-is (typically straight into
# `mcp_common::SetObjectProperty`'s generic JSON->UProperty importer, e.g.
# `set_component_property`/`set_blueprint_component_properties`'s `property_value`).
# These params genuinely accept ANY JSON type (string/number/bool/array/object) —
# they must NOT fall through to the "no hint -> string" default (see JSON_TYPE
# lookups below), or the generated tool schema wrongly rejects
# dict/list values, blocking any TMap/array/struct property from ever reaching
# the handler. Deliberately distinct from FIELD_RE: "GetField" has no type
# infix, so it never overlaps with `Get{String,Number,...}Field`.
RAW_FIELD_RE = re.compile(
    r'\bParams\s*->\s*(?:Try)?GetField\s*\(\s*TEXT\("([^"]+)"\)'
)
# `Params->HasField(TEXT("x"))` also signals a param (often an optional guarded read).
HASFIELD_RE = re.compile(r'\bParams\s*->\s*HasField\s*\(\s*TEXT\("([^"]+)"\)')
# A param is REQUIRED when its read is negated as a guard that fails the call, e.g.
#   if (!Params->TryGetStringField(TEXT("name"), X)) return MakeErrorReply(...)
# (a guard that only assigns a default does not count: see _negated_read_requires)
REQUIRED_RE = re.compile(
    r'!\s*Params\s*->\s*(?:Try)?Get'
    r'(?:String|Number|Integer|Bool|Array|Object|StringArray)Field'
    r'\s*\(\s*TEXT\("([^"]+)"\)'
)
# The untyped forms of that guard, for a param of any JSON type:
#   if (!Params->HasField(TEXT("value"))) return MissingParameterReply(TEXT("value"));
#   if (!Params->TryGetField(TEXT("value"))) return ...;
# They count only where the guard runs on every path of the handler (see _guard_runs_always): a raw
# read says nothing about a type, so the generator cannot tell a required value from an optional one
# the way it does for a typed read and its fallback, and leaves the param optional unless the guard
# is unconditional.
RAW_REQUIRED_RE = re.compile(
    r'!\s*Params\s*->\s*(?:HasField|(?:Try)?GetField)\s*\(\s*TEXT\("([^"]+)"\)'
)
# ...BUT two or more negated reads chained with `&&` INSIDE THE SAME guard are
# ALTERNATIVE spellings of the same parameter, not two requirements:
#   if (!Params->TryGetStringField(TEXT("data_table"), T) &&
#       !Params->TryGetStringField(TEXT("table_path"), T)) return MakeErrorReply(...)
# fails only if BOTH are missing, so ONE is enough. (Two real requirements are written
# with `||`, which does not match here.) Marking them all `required` would make a correct
# call IMPOSSIBLE: the schema would demand both names -- send one and it asks for the other.
# So ONE member of the chain stays `required` (`_prefer_documented` picks which) and the rest
# become OPTIONAL ALIASES, each marked `aliasOf` it (`_mark_aliases`). The server reads that
# mark to accept any spelling for the required one, which is what the handler does.
_NEG_READ_RE = re.compile(
    r'!\s*Params\s*->\s*(?:Try)?Get'
    r'(?:String|Number|Integer|Bool|Array|Object|StringArray)Field'
    r'\s*\(\s*TEXT\("([^"]+)"\)'
)
# Between the end of one negated read and the start of the next there may only be
# closing parentheses, whitespace/newlines and the `&&`. Anything else (`||`, the `)`
# that closes the if, a statement) breaks the chain: the next read belongs to another guard.
_CHAIN_GAP_RE = re.compile(r'^[\s)]*&&[\s]*$')


def _alias_reads(body: str) -> list[tuple[str, str]]:
    """(head, alias) pairs for each `&&` chain of negated reads.

    The HEAD is the chain's first read; the rest are its aliases. They are returned
    as pairs -- not as a bare set -- because which one keeps `required` is decided
    later against the annotation (see `_prefer_documented`): the required name must
    be the one the `@param` DOCUMENTS, not whichever the C++ happens to read first.
    Otherwise some chains come out backwards, with the required param undocumented
    and the documented one marked optional.
    """
    hits = list(_NEG_READ_RE.finditer(body))
    pairs: list[tuple[str, str]] = []
    head: str | None = None
    for prev, cur in zip(hits, hits[1:]):
        # the gap runs from the end of the previous TEXT("...") to the start of the current one
        gap = body[prev.end():cur.start()]
        # skip the rest of the previous read's arguments: `, Var)`
        cut = gap.find(")")
        if cut != -1 and _CHAIN_GAP_RE.match(gap[cut:]):
            head = head or prev.group(1)
            pairs.append((head, cur.group(1)))
        else:
            head = None
    return pairs


def _prefer_documented(params: list[dict], chains: dict[str, str]) -> None:
    """Move a chain's `required` flag to the FIRST member the annotation documents.

    `chains` maps each member (head included) to its chain key. Within a chain
    only ONE member can be required (the others are alternative spellings), and
    it is the first documented one in documented order (`params` lists the
    documented params first, in that order), whichever the C++ reads first.
    That way the name the schema REQUIRES is the one the annotation presents as
    the parameter, never an undocumented alias -- and documenting an alias
    AFTER the canonical name (`@param data_table - Alias of table_path.`) does
    not move the requirement onto the alias, which would make the server refuse
    every call that passes only the canonical name."""
    by_chain: dict[str, list[dict]] = {}
    for p in params:
        key = chains.get(p["name"])
        if key:
            by_chain.setdefault(key, []).append(p)
    for group_params in by_chain.values():
        req = next((p for p in group_params if p.get("required")), None)
        if not req:
            continue                      # no required member
        doc = next((p for p in group_params if (p.get("description") or "").strip()), None)
        if doc is not None and doc is not req:
            req["required"] = False
            doc["required"] = True


def _mark_aliases(params: list[dict], chains: dict[str, str]) -> None:
    """Mark each other spelling of a chain `aliasOf` the member that represents it.

    That member is the required one, or the first one published when no member is
    required. The flag stays on that member alone, so the server would refuse a call
    that passes only an alias although the handler accepts it: the server reads
    `aliasOf` (FMcpToolCatalog::LoadFromFile, Tools/catalog_server.py) to drop the
    member from the schema's `required` and to accept any spelling of it, as it does
    for the dispatcher's alias table."""
    by_chain: dict[str, list[dict]] = {}
    for p in params:
        key = chains.get(p["name"])
        if key:
            by_chain.setdefault(key, []).append(p)
    for members in by_chain.values():
        canonical = next((p for p in members if p.get("required")), members[0])
        for p in members:
            if p is not canonical:
                p["aliasOf"] = canonical["name"]


def assemble_params(extracted: list, ann_params: dict, domain: str = "") -> list[dict]:
    """The params a command publishes, built from the C++ alone.

    Every `@param` the annotation documents, in documented order, then the params
    the code reads that no `@param` names, in read order; required ones first
    (stable). Type and required come from the code when it reads the param (the
    regex is anchored to `Params`, so reads off result objects never count), and
    otherwise from the `[type!]` hint (default string/optional). Descriptions come
    from the annotation. Following the documented order keeps the schema stable
    when a handler moves its reads into a helper. A param every read of which
    lands in an integer variable also carries `intRange` (see _int_range), a
    string the engine makes a name of carries `nameLike` (see _is_name_like), and a
    param every read of which is one vector reader carries `vectorShape` (see
    _vector_shape)."""
    code: dict[str, tuple[str, bool]] = {}
    chains: dict[str, str] = {}
    ranges: dict[str, list[int]] = {}
    shapes: dict[str, str] = {}
    for item in extracted:
        pname, jt, req, chain_key = item[0], item[1], item[2], item[3]
        if pname not in code:
            code[pname] = (jt, req)
            if chain_key:
                chains[pname] = chain_key
            if len(item) > 4 and item[4]:
                ranges[pname] = list(item[4])
            if len(item) > 5 and item[5]:
                shapes[pname] = item[5]
    rows: list[tuple[str, str, bool, str]] = []
    for pname in [*ann_params, *(n for n in code if n not in ann_params)]:
        a = ann_params.get(pname)
        jt, req = code[pname] if pname in code else ((a["type"] or "string"), a["required"])
        rows.append((pname, jt, req, a["desc"] if a else ""))
    name_like = {pname for pname, jt, _req, desc in rows if _is_name_like(pname, jt, desc, domain)}
    # The other spellings of a param (its alias chain) carry what it carries.
    marked_chains = {chains[pname] for pname in name_like if pname in chains}
    name_like |= {pname for pname, jt, _req, _desc in rows
                  if chains.get(pname) in marked_chains and jt in _NAME_LIKE_TYPES}
    params: list[dict] = []
    for pname, jt, req, desc in rows:
        param = {"name": pname, "jsonType": jt, "required": req}
        if pname in ranges:
            param["intRange"] = ranges[pname]
        if pname in name_like:
            param["nameLike"] = True
        if pname in shapes:
            param["vectorShape"] = shapes[pname]
        param["description"] = desc
        params.append(param)
    # Within an alias chain, the REQUIRED name must be the one the `@param`
    # documents -- otherwise the schema demands an undocumented alias and leaves
    # optional the only one that says what it is for.
    _prefer_documented(params, chains)
    _mark_aliases(params, chains)
    params.sort(key=lambda p: not p.get("required", False))
    return params


JSON_TYPE = {
    "String": "string", "Number": "number", "Integer": "number",
    "Bool": "boolean", "Array": "array", "Object": "object",
    "StringArray": "array",
}
# Convenience aliases accepted in the `@param [hint]` type. The codebase de-facto
# writes [bool]/[int] (100% of hints), so normalize them to the canonical JSON
# Schema type — otherwise a hint-only param (one not read off the code) would emit
# an invalid jsonType "bool"/"int". Params read off the code already map via
# JSON_TYPE (GetBoolField -> "boolean"), so this just makes the two paths agree.
HINT_TYPE_ALIAS = {"bool": "boolean", "int": "integer"}
# Vector/rotator/color helpers (mcp_common::ReadJson{Vector,Vector2D,Rotator,LinearColor})
# read the field as a JSON array via TryGetArrayField and default to zero (the color: to
# its caller's default) when it is missing, so a param passed to one is an OPTIONAL array.
# The field name lives at the call site (the cross-file helper reads it through its
# FieldName arg, which the extractor cannot follow), so match the call site directly.
# Anchored to `Params` like FIELD_RE so reads off a sub-object are not mistaken for inputs.
VECTOR_HELPER_RE = re.compile(
    r'\bReadJson(?:Vector2D|Vector|Rotator|LinearColor)\s*\(\s*Params\s*,\s*TEXT\("([^"]+)"\)'
)
# mcp_common::ResolveWorldSelector(Params, ...) reads the optional string param `world`.
WORLD_SELECTOR_RE = re.compile(r'\bResolveWorldSelector\s*\(\s*Params\b')
# mcp_common::ReadRequired<Type>(Params, TEXT("x"), ...) reads the REQUIRED param `x` of that type.
REQUIRED_READ_RE = re.compile(r'\bReadRequired(String|Number|Bool)\s*\(\s*Params\s*,\s*TEXT\("([^"]+)"\)')

# ---------------------------------------------------------------------------
# MCP annotation parsing (single source of truth = the C++).
# ---------------------------------------------------------------------------
# Marker line: `// MCP:<cmd> - <description>` (or `//MCP:`, `//  MCP:`, etc.)
ANNOTATION_MARKER_RE = re.compile(r'^//\s*MCP:(\S+)\s+-\s+(.+)$')
# @param line: `//   @param <name> [<type>!] - <description>` — 3+ spaces after `//`.
# The optional `[<type>!]` hint DECLARES the JSON type (string/number/integer/
# boolean/array/object) and, with a trailing `!`, that the param is required.
# It is needed only for params the C++ extractor cannot see (read via cross-file
# helpers, or not read off `Params` at all) so the annotation alone is
# authoritative for them. Without a hint, type/required come from code
# extraction, defaulting to string/optional. Groups: name, type, '!'?, desc.
ANNOTATION_PARAM_RE  = re.compile(
    r'^//\s{3,}@param\s+(\S+)(?:\s+\[([a-zA-Z]*)(!?)\])?\s+-\s+(.+)$')
# @returns line: `//   @returns <description or { shape }>`
ANNOTATION_RETURNS_RE = re.compile(r'^//\s{3,}@returns\s+(.+)$')
# @para line: `//   @para <text>` -- one more paragraph of the description, after a blank line: when to use the tool and
# what it does that the first paragraph does not say, so a long description reads as short paragraphs. One line each.
ANNOTATION_PARA_RE = re.compile(r'^//\s{3,}@para\s+(.+)$')
# @renamed-from line: `//   @renamed-from <old_name>` -- a name the tool had before. The catalog's `renamed`
# maps it to this tool: a call to the old name runs this one and the reply says so, and tools/list leaves it out.
ANNOTATION_RENAMED_RE = re.compile(r'^//\s{3,}@renamed-from\s+([a-z0-9_]+)\s*$')
# @retired line: `//   @retired <listed_tool> - <how to make the same call with it>` -- the tool leaves tools/list
# and still runs under its own name; the reply names the listed tool that does the same, and how.
ANNOTATION_RETIRED_RE = re.compile(r'^//\s{3,}@retired\s+([a-z0-9_]+)\s+-\s+(.+)$')
# @core line: `//   @core` -- the tool is in the core list, the one tools/list carries by default (FMcpToolCatalog's
# core list); the others are reached with find_tools, call_tool and enable_toolset.
ANNOTATION_CORE_RE = re.compile(r'^//\s{3,}@core\s*$')
# The core list stays small: it is what every agent pays for in context, and what a directory scores. With the four
# meta-tools it ends with, 11 core tools make a list of 15; a directory scores a longer list as too long for an agent.
MAX_CORE_TOOLS = 11
# @pack line: `//   @pack <name>` -- the tool belongs to that pack, a small set of tools enable_toolset adds by name for
# a job the core list leaves out (FMcpToolCatalog's packs). A tool is in one pack at most, and a core tool in none.
ANNOTATION_PACK_RE = re.compile(r'^//\s{3,}@pack\s+([a-z0-9_]+)\s*$')
# The packs, in the order find_tools lists them, and the job each one is for: find_tools and enable_toolset publish it.
# A pack name is never a domain name, so enable_toolset and ?domains= take either without ambiguity.
PACKS = {
    "actors": "find, spawn and delete the actors of the open level",
    "levels": "open, save and inspect levels, and list what is unsaved",
    "play": "start, stop and check Play-in-Editor, and capture the viewport",
    "assets": "delete and duplicate assets, and find what references one",
    "blueprint_authoring": "create Blueprints, add variables, read their outline, unlink pins, remove nodes and build several "
                           "wired nodes at once",
    "data_tables": "read a data table row and change its fields",
}
# Any other line inside an annotation block (skip silently).
ANNOTATION_OTHER_RE   = re.compile(r'^//')
# `// MCP-DOMAIN: <domain>` — declares a handler class's MCP domain (the domain
# is sourced from the C++, not from a hand-maintained table).
ANNOTATION_DOMAIN_RE  = re.compile(r'^//\s*MCP-DOMAIN:\s*(\S+)\s*$', re.M)

def _comment_indent(line: str) -> int:
    """How many spaces separate a comment line's `//` from its text."""
    body = line[2:]
    return len(body) - len(body.lstrip(" "))


def _unclosed_shape(text: str) -> bool:
    """Whether `text` leaves a `{ ... }` or `[ ... ]` open: the shape goes on below."""
    return text.count("{") != text.count("}") or text.count("[") != text.count("]")


def find_truncated_annotations(text: str) -> list[tuple[str, str, int, str]]:
    """Find annotation text that does not reach the catalog in full.

    `description`, `@param` and `@returns` are ONE line each (all three regexes end
    in `(.+)$`). Any following `//` line that matches none of them is swallowed by
    ANNOTATION_OTHER_RE without a warning: `--check` stays green and needs_annotation
    stays at 0, because there IS a description -- just cut off mid-sentence. What is
    lost is often the discriminating part ("use this one and not connect_pins because...").

    `description` and `@param` are reported whenever the next line goes on. `@returns`
    is reported only when it is cut for certain, because implementation notes are parked
    BELOW it by convention, at the tag's indentation, where the parser ignores them on
    purpose. It is cut when its braces or brackets do not close on its line, or when the
    next line is indented deeper than the tag (a continuation aligned under the text).
    A second `@returns` in the same block is reported too: the parser keeps the last one,
    so the first never reaches the catalog.

    Returns: [(cmd, kind, lineno_1based, how_it_is_lost)]
    """
    out: list[tuple[str, str, int, str]] = []
    lines = text.splitlines()
    discarded = "continues on the next line, which the parser DISCARDS -> lost"

    def is_continuation(nxt: str) -> bool:
        if not ANNOTATION_OTHER_RE.match(nxt):
            return False
        if (ANNOTATION_PARAM_RE.match(nxt) or ANNOTATION_RETURNS_RE.match(nxt) or ANNOTATION_PARA_RE.match(nxt)
                or ANNOTATION_RENAMED_RE.match(nxt) or ANNOTATION_RETIRED_RE.match(nxt)
                or ANNOTATION_CORE_RE.match(nxt) or ANNOTATION_PACK_RE.match(nxt)
                or ANNOTATION_MARKER_RE.match(nxt) or ANNOTATION_DOMAIN_RE.match(nxt)
                or tool_hints.HINTS_LINE_RE.match(nxt)):
            return False
        return nxt.strip() not in ("//", "//---", "// ---")

    i = 0
    while i < len(lines):
        m = ANNOTATION_MARKER_RE.match(lines[i])
        if not m:
            i += 1
            continue
        cmd = m.group(1)
        returns_at: int | None = None
        j = i
        while j < len(lines) and ANNOTATION_OTHER_RE.match(lines[j]):
            if j > i and ANNOTATION_MARKER_RE.match(lines[j]):
                break
            kind = ("description" if j == i
                    else "@param" if ANNOTATION_PARAM_RE.match(lines[j])
                    else "@para" if ANNOTATION_PARA_RE.match(lines[j])
                    else "@retired" if ANNOTATION_RETIRED_RE.match(lines[j]) else None)
            nxt = lines[j + 1] if j + 1 < len(lines) else ""
            if kind and is_continuation(nxt):
                out.append((cmd, kind, j + 1, f'{discarded}: "{nxt.strip()[:70]}"'))
            rm = ANNOTATION_RETURNS_RE.match(lines[j])
            if rm:
                if returns_at is not None:
                    out.append((cmd, "@returns", returns_at + 1,
                                f"is replaced by the @returns at line {j + 1}, because the parser keeps "
                                f'the last one -> lost: "{lines[returns_at].strip()[:70]}"'))
                returns_at = j
                goes_on = is_continuation(nxt)
                if goes_on and (_unclosed_shape(rm.group(1))
                                or _comment_indent(nxt) > _comment_indent(lines[j])):
                    out.append((cmd, "@returns", j + 1, f'{discarded}: "{nxt.strip()[:70]}"'))
                elif _unclosed_shape(rm.group(1)):
                    out.append((cmd, "@returns", j + 1,
                                "leaves a brace or bracket open, and nothing below closes it"))
            j += 1
        i = j
    return out


def parse_annotations(text: str) -> dict[str, dict]:
    """Parse `// MCP:` annotation blocks scattered through a .cpp file.

    Returns: {cmd: {"description": str, "params": {name: {"desc", "type", "required"}}, "returns": str,
              "renamedFrom": [old names], "retired": {"use", "how"} or None, "core": bool}}.

    A block is a contiguous run of `//` comment lines that starts with a marker
    `// MCP:<cmd> - <desc>` and ends at the first non-comment line. Anything
    inside the block matching `//   @param <name> - <desc>` or
    `//   @returns <text>` populates the entry. Multiple blocks for the SAME cmd
    in the same file keep the FIRST one and warn for the rest (duplicates can
    appear through manual editing). Unknown commands (not in any dispatch) are
    kept but reported by the caller.
    """
    out: dict[str, dict] = {}
    seen_dup: dict[str, int] = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = ANNOTATION_MARKER_RE.match(line)
        if not m:
            i += 1
            continue
        cmd, desc = m.group(1), m.group(2).rstrip()
        entry: dict = {"description": desc, "params": {}, "returns": "", "renamedFrom": [], "retired": None, "core": False,
                       "pack": None}
        j = i + 1
        while j < len(lines):
            ln = lines[j]
            if not ANNOTATION_OTHER_RE.match(ln):
                break  # end of block: next line is code
            # A new `// MCP:<cmd>` marker ENDS this block (start of next).
            # Without this guard, params from the next cmd leak into the previous
            # block's entry when annotations sit adjacent to one another.
            if ANNOTATION_MARKER_RE.match(ln):
                break
            pm = ANNOTATION_PARAM_RE.match(ln)
            if pm:
                # group(1)=name, group(2)=type hint (may be ''), group(3)='!'?,
                # group(4)=description. Insertion order = documented order.
                _hint = (pm.group(2) or "").strip().lower()
                entry["params"][pm.group(1)] = {
                    "desc": pm.group(4).rstrip(),
                    "type": (HINT_TYPE_ALIAS.get(_hint, _hint) or None),
                    "required": (pm.group(3) == "!"),
                }
            elif (nm := ANNOTATION_RENAMED_RE.match(ln)):
                entry["renamedFrom"].append(nm.group(1))
            elif (tm := ANNOTATION_RETIRED_RE.match(ln)):
                entry["retired"] = {"use": tm.group(1), "how": tm.group(2).rstrip()}
            elif ANNOTATION_CORE_RE.match(ln):
                entry["core"] = True
            elif (km := ANNOTATION_PACK_RE.match(ln)):
                entry["pack"] = km.group(1)
            elif (am := ANNOTATION_PARA_RE.match(ln)):
                entry["description"] += "\n\n" + am.group(1).rstrip()
            else:
                rm = ANNOTATION_RETURNS_RE.match(ln)
                if rm:
                    entry["returns"] = rm.group(1).rstrip()
            j += 1
        if cmd in out:
            seen_dup[cmd] = seen_dup.get(cmd, 1) + 1
        else:
            out[cmd] = entry
        i = j
    return out, seen_dup

# ---------------------------------------------------------------------------
# Naming: CamelCase -> snake_case, with acronym preservation
# ---------------------------------------------------------------------------
ACRONYMS = [
    "NavMesh", "APIDocs",        # multi-word
    "FOV", "FPS", "HUD", "API", "IK", "PCG", "HLOD", "LOD", "LODs",
    "GC", "RT", "MID", "MIDs", "BP", "ID", "URL", "JSON", "CSV", "URI",
    "UI", "HTTP", "HTTPS", "XML", "YAML", "PNG", "JPG", "JPEG", "MIDI",
    "GPU", "CPU", "FSM", "BT", "OSC", "OSC", "VaRest",
    # digits handled separately
]

def to_snake(name: str) -> str:
    """CamelCase -> snake_case, preserving acronyms.
    GetCameraFOV -> get_camera_fov
    ConfigureAudio3D -> configure_audio_3d
    SpawnNavMeshBoundsVolume -> spawn_nav_mesh_bounds_volume
    """
    s = name
    # Insert underscore between letter and digit
    s = re.sub(r"([A-Za-z])(\d)", r"\1_\2", s)
    # Insert underscore between digit and letter
    s = re.sub(r"(\d)([A-Za-z])", r"\1_\2", s)
    # Insert underscore at acronym->word boundary (FOOBar -> FOO_Bar)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s)
    # Insert underscore at camelCase boundary (aB -> a_B)
    s = re.sub(r"([a-z\d])([A-Z])", r"\1_\2", s)
    return s.lower()

def validate_snake(s: str) -> list[str]:
    """Heuristic: a 'good' snake_case shouldn't have single-letter fragments
    like `_x_y_` that look like an acronym that was mis-split (f_o_v, h_u_d,
    f_p_s, a_p_i). Returns a list of warning strings (empty = OK)."""
    warnings = []
    # Split on _ and find 1-char tokens that are sandwiched between other
    # tokens (likely acronyms).
    parts = s.split("_")
    for i, p in enumerate(parts):
        if len(p) == 1 and p.isalpha() and 0 < i < len(parts) - 1:
            warnings.append(
                f"possible acronym split: '{s}' has 1-letter token '{p}' at pos {i} "
                f"(consider collapsing to '{s[:s.find('_'+p+'_')]}{p.upper()}{s[s.find('_'+p+'_')+3:]}')"
            )
    return warnings

# ---------------------------------------------------------------------------
# Handler .cpp parsing
# ---------------------------------------------------------------------------
def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return text


# A call that passes `Params` as its FIRST argument is a forwarder/helper whose
# body reads the real params, e.g. `HandleCreateWaterBody(Params, River)` or
# `ResolveSkeleton(Params, Out)`. (`Params->TryGetField(...)` does NOT match —
# there Params is the object and the first arg is TEXT(...).) A call with
# template arguments, `HandleRemoveTyped<UFoo>(Params, ...)`, is followed too:
# group 1 is the function name, and its definition is found by that name.
CALLEE_RE = re.compile(r'\b([A-Za-z_]\w*)\s*(?:<[^<>;(){}]*>\s*)?\(\s*Params\b')
# Control-flow keywords match CALLEE_RE (`if (Params->...)`) but are NOT calls;
# following them would scan an unrelated control block elsewhere in the file.
_CPP_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "do",
                 "else", "sizeof", "static_cast", "reinterpret_cast", "const_cast"}


def _body_from_brace(text: str, brace: int) -> str:
    depth = 0
    for j in range(brace, len(text)):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[brace:j + 1]
    return text[brace:]


def _find_function_body(text: str, name: str) -> str:
    """Return the brace-balanced body of the DEFINITION of `name`, or ''.

    Handles both methods (`FClass::name(...) {`) and free/static helpers
    (`Type name(...) {`). A definition is a `name(...)` whose matching `)` is
    followed by `{`; a call (`name(...);`) is skipped."""
    return _find_function_def(text, name)[1]


def _find_function_def(text: str, name: str) -> tuple[str, str]:
    """(parameter list, body) of the DEFINITION of `name`, or ('', ''): the list,
    parentheses included, is where an out-parameter such as `int32& OutIndex` is declared."""
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', text):
        depth = 0
        j = m.end() - 1                      # at the '('
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        k = j + 1
        while k < len(text) and text[k] in " \t\r\n":
            k += 1
        if k < len(text) and text[k] == "{":  # ')' followed by '{' => definition
            return text[m.end() - 1:j + 1], _body_from_brace(text, k)
    return "", ""


_IF_OPEN_RE = re.compile(r'\bif\s*\(')


def _guarded_statement(body: str, pos: int) -> str | None:
    """The statement guarded by the `if` whose CONDITION holds the read at `pos`, or
    None when that read is not inside an if-condition (a ternary, an assignment...)."""
    for m in reversed([m for m in _IF_OPEN_RE.finditer(body, 0, pos)]):
        open_at = m.end() - 1
        depth, k = 0, open_at
        while k < len(body):
            if body[k] == "(":
                depth += 1
            elif body[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if not (open_at < pos < k):
            continue  # pos is past this if's condition: try the enclosing one
        rest = body[k + 1:]
        lead = len(rest) - len(rest.lstrip())
        if rest[lead:lead + 1] == "{":
            depth = 0
            for n in range(lead, len(rest)):
                if rest[n] == "{":
                    depth += 1
                elif rest[n] == "}":
                    depth -= 1
                    if depth == 0:
                        return rest[lead:n + 1]
            return rest[lead:]
        end = rest.find(";")
        return rest[:end + 1] if end != -1 else rest
    return None


# Any read off `Params` with its output argument: `Params->TryGetStringField(TEXT("x"), Out)`.
_READ_INTO_RE = re.compile(
    r'Params\s*->\s*(?:Try)?Get'
    r'(?:String|Number|Integer|Bool|Array|Object|StringArray)Field'
    r'\s*\(\s*TEXT\("([^"]+)"\)\s*,\s*([^,()]+?)\s*\)'
)


def _read_target(body: str, pos: int) -> str | None:
    """The output argument of the read that starts at `pos`, or None."""
    m = _READ_INTO_RE.match(body, body.find("Params", pos))
    return m.group(2).strip() if m else None


# The integer types a JSON number is read into, with the values FJsonValue::TryGetNumber
# accepts for each. Outside that range the read FAILS and the handler goes on with the
# variable's default, so a number outside int32 silently became 0, -1 or the first slot.
# The catalog publishes the range (`intRange`) of every number param whose reads all land
# in such a variable, and the dispatcher refuses a value outside it before the handler runs
# (McpPreconditions, check "integer_range").
_INT_RANGES = {
    "int8": (-128, 127), "uint8": (0, 255),
    "int16": (-32768, 32767), "uint16": (0, 65535),
    "int32": (-2147483648, 2147483647), "int": (-2147483648, 2147483647),
    "uint32": (0, 4294967295),
}
_NUMERIC_TYPE_ALT = "|".join((*_INT_RANGES, "int64", "uint64", "float", "double", "bool"))
_NUM_READ_RE = re.compile(r'\bParams\s*->\s*(Try)?Get(Number|Integer)Field\s*\(\s*TEXT\("([^"]+)"\)')
# mcp_common::ReadRequiredNumber is a TryGetNumberField into its third argument, whatever its type.
_REQUIRED_NUM_READ_RE = re.compile(r'\bReadRequiredNumber\s*\(\s*Params\s*,\s*TEXT\("([^"]+)"\)\s*,\s*(\w+)\s*\)')


def _declared_type(scope: str, target: str) -> str | None:
    """The type `target` is declared with in `scope` (a function's parameter list and body),
    or None when there is no declaration, or several of different types."""
    if not re.fullmatch(r'[A-Za-z_]\w*', target):
        return None  # a struct member or an array element: no declaration to read
    name = re.escape(target)
    found = {m.group(1) for m in re.finditer(rf'\b({_NUMERIC_TYPE_ALT})\s*&?\s*\b{name}\b(?!\s*\()', scope)}
    # a later name of a multiple declaration: `double Roll = 0, Pitch = 0, Yaw = 0;`
    later = rf'\b({_NUMERIC_TYPE_ALT})\s+\w+\s*(?:=\s*[^;,(){{}}]*)?(?:,\s*\w+\s*(?:=\s*[^;,(){{}}]*)?)*,\s*{name}\b'
    found |= {m.group(1) for m in re.finditer(later, scope)}
    return found.pop() if len(found) == 1 else None


def _numeric_read_type(scope: str, body: str, m: re.Match) -> str | None:
    """The C++ type a `Params->(Try)Get{Number,Integer}Field` read (match of _NUM_READ_RE in
    `body`) lands in, or None when it cannot be told."""
    if m.group(2) == "Integer":
        return "int32"  # FJsonObject::GetIntegerField casts the double to int32
    if m.group(1):
        target = _read_target(body, m.start())
        return _declared_type(scope, target) if target else None
    before = body[body.rfind("\n", 0, m.start()) + 1:m.start()]
    decl = re.search(rf'\b({_NUMERIC_TYPE_ALT})\s+\w+\s*=\s*(?:static_cast<\s*\w+\s*>\s*\(\s*)?$', before)
    if decl:
        return decl.group(1)
    cast = re.search(r'\(\s*(' + "|".join(_INT_RANGES) + r')\s*\)\s*$', before)
    if cast:
        return cast.group(1)
    if re.search(r'\bFMath::(?:Round|Floor|Ceil|Trunc)ToInt(?:32)?\s*\(\s*$', before):
        return "int32"
    return None


def _int_range(types: list) -> tuple[int, int] | None:
    """The values every read can hold when ALL of them land in an integer variable, else None."""
    if not types or any(t not in _INT_RANGES for t in types):
        return None
    return max(_INT_RANGES[t][0] for t in types), min(_INT_RANGES[t][1] for t in types)


# An FName holds at most NAME_SIZE - 1 (1023) characters, and building one from a longer string
# is a checkf in UnrealNames.cpp that aborts the editor. The strings a tool resolves or creates as
# an asset, actor, class, property, tag, bone or key become FNames on the way (FName(*X),
# CreatePackage, LoadObject, FSoftObjectPath, FKey), so the catalog marks each param that carries
# one (`nameLike`), and the dispatcher refuses such a string, or a string element of such an array,
# of NAME_SIZE - 64 (960) characters or more before the handler runs (McpPreconditions, check
# "name_length"): a tool may add up to 64 characters around a name before it makes an FName of it
# (a class prefix, "/Game/Blueprints/", "/Script/Niagara.NiagaraDataInterface", "_Sub"...).
#
# The same rule, without the description half, judges the keys of objects INSIDE an argument (a
# rename row, a node spec): the catalog publishes it as `nameLikeRule` (name_like_rule), and the
# dispatcher refuses a string that long under a key it passes, at any depth (FMcpToolCatalog::
# IsNameLikeKey). A name is split into words at "_" and at each camelCase hump (_split_words), so a
# nested "newName" or "AssetPath" is judged like new_name and asset_path.
#
# The marks also bound a pair: the longest name-like argument that is a content path (it starts with
# "/") plus the longest other name-like string plus 160 must stay under NAME_SIZE, since an object a
# tool creates inside an asset is named after both ("<asset>_C:<name>_GEN_VARIABLE") and recording
# its creation makes an FName of that path (FMcpToolCatalog::SubobjectPathMargin).
#
# The param's NAME says what it carries: its last word, once a trailing number or single letter is
# dropped (object_path_1, table_a), is one of _NAME_WORDS; or its description calls it an FName, an
# FKey or a key name. Left out on purpose, as strings the engine keeps without that limit: disk
# paths (a word of _DISK_WORDS, an output or export path, or a description that shows a disk path),
# the USD tools' prim paths, layer identifiers and tokens (the USD domain, but for the actor it
# spawns), and free text, whose last word is never a name word (code, json, text, description,
# tooltip, comment, value, query, pattern, filter...) and never counts by its description either.
_NAME_LIKE_TYPES = frozenset({"string", "array", "any"})
_NAME_WORDS = frozenset({
    "name", "names", "label", "labels", "path", "paths", "class", "classes", "tag", "tags",
    "id", "ids", "guid", "namespace", "socket", "bone", "slot", "section", "property",
    "variable", "function", "event", "asset", "assets", "package", "folder", "map", "level",
    "actor", "actors", "component", "components", "curve", "track", "state", "node", "pin",
    "graph", "struct", "enum", "channel", "layer", "parameter", "blueprint", "bp", "material",
    "materials", "mesh", "texture", "cubemap", "skeleton", "chain", "table", "tables", "widget",
    "widgets", "dispatcher", "interface", "library", "macro", "column", "input", "attribute",
    "world", "handle", "type", "mode", "profile", "primitive", "action", "composite", "group",
    "trigger", "symbol",
})
_FREE_TEXT_WORDS = frozenset({
    "filter", "pattern", "contains", "match", "prefix", "value", "values", "text", "json", "string",
    "description", "comment", "message", "title", "tooltip", "script", "code", "query", "csv",
    "content", "reason",
})
_DISK_WORDS = frozenset({"file", "filename", "files", "dir", "directory", "uproject", "url"})
_DISK_PATH_SUFFIXES = ("output_path", "export_path", "rel_path")
_DISK_PATH_DOC_RE = re.compile(
    r'\b[A-Za-z]:[/\\]|^Absolute\b|(?<!not a )\b[Dd]isk (?:path|dir)|\bon disk \(|\bOS path|\b[Ff]ile ?system path')
_NAME_DOC_RE = re.compile(r'\bFName\b|\bFKey\b|\b[Kk]ey name\b')
_NOT_NAME_DOMAINS = frozenset({"USD"})
_NOT_NAME_DOMAIN_KEEP_WORD = "actor"  # in those domains a param naming the actor a tool spawns is still a name


def name_like_rule() -> dict:
    """The name half of _is_name_like, as the catalog publishes it (`nameLikeRule`) for the dispatcher's check of
    the keys inside an argument, which have no description."""
    return {
        "nameWords": sorted(_NAME_WORDS),
        "diskWords": sorted(_DISK_WORDS),
        "diskPathSuffixes": list(_DISK_PATH_SUFFIXES),
        "notNameDomains": sorted(_NOT_NAME_DOMAINS),
        "notNameDomainKeepWord": _NOT_NAME_DOMAIN_KEEP_WORD,
    }


def _split_words(name: str) -> list[str]:
    """The words of a name, in lower case: split at each "_" (empty words kept) and at each camelCase hump, before an
    upper-case letter that follows a lower-case letter or a digit, or that starts a word after a run of capitals
    ("newName" -> new, name; "HTTPServer" -> http, server). FMcpToolCatalog::IsNameLikeKey splits the same way."""
    words: list[str] = []
    for part in name.split("_"):
        word = ""
        for i, c in enumerate(part):
            if c.isupper() and word:
                previous = part[i - 1]
                next_is_lower = i + 1 < len(part) and part[i + 1].islower()
                if previous.islower() or previous.isdigit() or (previous.isupper() and next_is_lower):
                    words.append(word.lower())
                    word = ""
            word += c
        words.append(word.lower())
    return words


def _is_name_like(name: str, json_type: str, description: str, domain: str) -> bool:
    """Whether the param carries a name the engine makes an FName of (see the block above)."""
    if json_type not in _NAME_LIKE_TYPES:
        return False
    words = _split_words(name)
    while len(words) > 1 and (words[-1].isdigit() or len(words[-1]) == 1):
        words.pop()
    if domain in _NOT_NAME_DOMAINS and _NOT_NAME_DOMAIN_KEEP_WORD not in words:
        return False
    if any(w in _DISK_WORDS for w in words) or name.endswith(_DISK_PATH_SUFFIXES) or _DISK_PATH_DOC_RE.search(description):
        return False
    if words[-1] in _NAME_WORDS:
        return True
    return words[-1] not in _FREE_TEXT_WORDS and bool(_NAME_DOC_RE.search(description))


# A vector param (`vectorShape`). mcp_common::ReadJsonVector, ReadJsonVector2D and ReadJsonRotator read an array of
# 3 numbers ([X, Y, Z]), of 2 ([X, Y]) and, for the rotator, also an object of roll, pitch and yaw, and turn ANY other
# value into zero: a string, an object, a short array or a string element became the origin, a zero scale or a zero
# rotation while the call answered success. The catalog marks a param with its reader's shape ("vector", "vector2d",
# "rotator") when every read of its value is that reader, and the dispatcher refuses a present value of another shape
# before the handler runs (McpPreconditions, check "vector_shape"). An absent one keeps the tool's default; a null is
# refused, since a handler that tests the param with HasField takes it as present and reads it as zero (tools/call
# drops a null argument before the dispatch, so this concerns a batch item's params and in-process calls).
#
# Every `TEXT("p")` of the scanned bodies is a read of p's value except a presence test (`Params->HasField`), a write
# into a reply or a read of another object's field (any receiver but Params), an error naming it and
# AddRequestedEcho's list. So a param also read another way -- a raw array or a typed read, a validator of its own, a
# string compare -- is not marked, and neither is any param of a command that hands Params to code the scan does not
# read (_hands_params_unscanned), since that code could read it another way.
_VECTOR_SHAPES = {"Vector": "vector", "Vector2D": "vector2d", "Rotator": "rotator"}
_TEXT_LITERAL_RE = re.compile(r'TEXT\("([^"]+)"\)')
_VECTOR_READ_BEFORE_RE = re.compile(r'\bReadJson(Vector2D|Vector|Rotator)\s*\(\s*([\w*]+)\s*,\s*$')
_PRESENCE_BEFORE_RE = re.compile(r'\bParams\s*->\s*HasField\s*\(\s*$')
_MISSING_BEFORE_RE = re.compile(r'\bMissingParameterReply\s*\(\s*$')
_RECEIVER_BEFORE_RE = re.compile(r'\b(\w+)\s*(?:->|\.)\s*(?:Set|TryGet|Get|Has)\w*(?:\s*<[^<>;]*>)?\s*\(\s*$')
_ADD_FIELD_BEFORE_RE = re.compile(r'\bAdd\w*Field\s*\(\s*(\w+)\s*,\s*$')
_ECHO_LIST_RE = re.compile(r'\bAddRequestedEcho\s*\(\s*\w+\s*,\s*Params\s*,\s*\{([^{}]*)\}')
# Params handed over as a value: not dereferenced (`Params->`, `Params.`) and not a member of something else.
_PARAMS_VALUE_RE = re.compile(r'(?<![\w>.:])Params\b(?!\s*(?:->|\.))')
_PARAMS_VALUES_RE = re.compile(r'\bParams\s*->\s*Values\b')
# The mcp_common readers whose fields are literal arguments at the call site, or a fixed name that is never a vector:
# handing Params to one hides no read of a vector param.
_LITERAL_FIELD_READERS = frozenset({
    "ReadJsonVector", "ReadJsonVector2D", "ReadJsonRotator", "ReadJsonLinearColor", "ReadJsonFloatArray", "ReadRequiredString",
    "ReadRequiredNumber", "ReadRequiredBool", "ResolveWorldSelector", "ResolveWorldFromParams", "ExtractStringPath",
    "AddRequestedEcho",
    # McpGraphRouter::Route (Commands/Common/McpGraphRouter.cpp) hands a graph tool's call on another graph asset to
    # that asset's tool: it reads node_position as [x, y], as the handler's own ReadPosition does, and the other params
    # it reads are declared with @param type hints. So the shapes the handler's own reads give still hold.
    "Route",
})
_UNSEEN = ""  # the key of vec_reads that says some code reads Params unseen (no param has an empty name)


def _value_reads(body: str) -> list[tuple[str, str]]:
    """(param, how) for each `TEXT("param")` of `body` that reads a param's value (see the block above): how is the
    vector reader's shape for a read through ReadJsonVector, ReadJsonVector2D or ReadJsonRotator on Params, and
    "other" for any other read."""
    echo = [(m.start(1), m.end(1)) for m in _ECHO_LIST_RE.finditer(body)]
    reads = []
    for t in _TEXT_LITERAL_RE.finditer(body):
        before = body[max(0, t.start() - 160):t.start()]
        if any(s <= t.start() < e for s, e in echo) or _PRESENCE_BEFORE_RE.search(before) \
                or _MISSING_BEFORE_RE.search(before):
            continue
        vector = _VECTOR_READ_BEFORE_RE.search(before)
        if vector:
            if vector.group(2) == "Params":
                reads.append((t.group(1), _VECTOR_SHAPES[vector.group(1)]))
            elif re.fullmatch(r'\w+', vector.group(2)):
                continue  # the same reader on another object: a field of that object
            else:
                reads.append((t.group(1), "other"))
            continue
        receiver = _RECEIVER_BEFORE_RE.search(before) or _ADD_FIELD_BEFORE_RE.search(before)
        if receiver and receiver.group(1) != "Params":
            continue
        reads.append((t.group(1), "other"))
    return reads


def _enclosing_call(code: str, pos: int) -> tuple[str | None, int]:
    """(name, open) of the call whose argument list holds `pos` (literals blanked), or (None, -1) when `pos` is not
    inside one within its statement."""
    depth, k = 0, pos - 1
    while k >= 0:
        c = code[k]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                m = re.search(r'([A-Za-z_]\w*)\s*(?:<[^<>;(){}]*>\s*)?$', code[:k])
                return (m.group(1) if m else None), k
            depth -= 1
        elif c in ";{}" and depth == 0:
            break
        k -= 1
    return None, -1


@lru_cache(maxsize=None)
def _is_defined(text: str, name: str) -> bool:
    """Whether `text` holds the definition of `name` (memoized: asked at every hand-over of Params)."""
    return bool(_find_function_def(text, name)[1])


def _hands_params_unscanned(text: str, body: str, visited: set) -> bool:
    """Whether `body` hands Params to code the scan does not read: its values (`Params->Values`), a call that takes it
    as a later argument or dereferenced, a function the scan did not follow (`visited` holds the ones it reached; one
    not defined in `text` lives in another file, one past the depth limit was never reached), or a name it is assigned
    to. The literal-field readers, control keywords and a lambda capture do not count."""
    if _PARAMS_VALUES_RE.search(body):
        return True
    code = _blank_literals(body)
    for m in _PARAMS_VALUE_RE.finditer(code):
        before = code[:m.start()].rstrip()
        if before.endswith(("[", "[&", "[=")) or re.search(r'\[[^\[\]();]*$', before):
            continue  # a lambda capture: the lambda's body is part of this body
        callee, open_at = _enclosing_call(code, m.start())
        if callee is None:
            return True
        if callee in _CPP_KEYWORDS or callee in _LITERAL_FIELD_READERS:
            continue
        first = not code[open_at + 1:m.start()].strip()
        if not first or callee not in visited or not _is_defined(text, callee):
            return True
    return False


def _vector_shape(reads: list) -> str | None:
    """The shape of the one vector reader every read of a param goes through, else None."""
    kinds = set(reads)
    return kinds.pop() if len(kinds) == 1 and kinds <= set(_VECTOR_SHAPES.values()) else None


def _missing_value_fails(body: str, start: int, target: str) -> bool:
    """Whether, after `start`, the handler fails the call when `target` is still empty or null:
    an if-condition testing it (`X.IsEmpty()`, `!X`, `X == nullptr`) whose statement returns.
    An alias chain that ends without such a check falls back to a default (e.g. a helper that
    picks a folder), so its head is optional."""
    name = re.escape(target)
    empty_test = re.compile(rf'(!?)\s*\b{name}\s*\.\s*IsEmpty\s*\(\s*\)')
    null_test = re.compile(rf'!\s*{name}\b(?!\s*(?:\.|->))|\b{name}\s*==\s*nullptr')

    def tests_missing(cond: str) -> bool:
        # `X.IsEmpty()` counts, `!X.IsEmpty()` (it HAS a value) does not; so do `!X` and `X == nullptr`.
        return any(not e.group(1) for e in empty_test.finditer(cond)) or bool(null_test.search(cond))

    for m in _IF_OPEN_RE.finditer(body, start):
        open_at = m.end() - 1
        depth, k = 0, open_at
        while k < len(body):
            if body[k] == "(":
                depth += 1
            elif body[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if not tests_missing(body[open_at:k + 1]):
            continue
        guarded = _guarded_statement(body, open_at + 1)
        if guarded is not None and re.search(r'\breturn\b', guarded):
            return True
    return False


_STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')
_LAMBDA_OPEN_RE = re.compile(r'\]\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:mutable\s*)?(?:->\s*[\w:<>,\s*&]+?)?\s*\{')
_RETURN_RE = re.compile(r'\breturn\b([^;]*);')
# What an error return reads like: an error reply built in place, or a variable holding one
# (`return Missing;`, `return Error;`, `return LoadError;`, `return Refused;`, `return NoWorld;`).
_ERROR_RETURN_RE = re.compile(
    r'MakeErrorReply|MissingParameterReply|EmptyParamError|AssetNotFoundError|RangeError|'
    r'\b\w*(?:Missing|Error|Err|Refus|Invalid|NoWorld|Fail|Bad|Problem|Conflict|Complaint|Unsupported)\w*\b')


def _blank_literals(body: str) -> str:
    """`body` with the inside of every string and char literal blanked, same length: braces, parentheses and
    `return` inside a message must not count as code."""
    return _STRING_LITERAL_RE.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[-1], body)


def _lambda_spans(code: str) -> list[tuple[int, int]]:
    """(start, end) of each lambda body in `code` (literals already blanked)."""
    spans = []
    for m in _LAMBDA_OPEN_RE.finditer(code):
        open_at = m.end() - 1
        spans.append((open_at, open_at + len(_body_from_brace(code, open_at))))
    return spans


def _is_error_return(code: str, original: str, ret: re.Match) -> bool:
    """Whether the `return` matched by `ret` (_RETURN_RE over `code`) hands back an error: an error reply built in
    place or held by an error-named variable (_ERROR_RETURN_RE), or the reply an `if (TSharedPtr<FJsonObject> X =
    Check(...)) { return X; }` guard declares, the plugin's idiom for a check that answers null when it passes."""
    expr = original[ret.start(1):ret.end(1)].strip()
    if _ERROR_RETURN_RE.search(expr):
        return True
    return bool(re.fullmatch(r'\w+', expr)) and bool(re.search(
        r'\bif\s*\(\s*(?:const\s+)?TSharedPtr\s*<\s*FJsonObject\s*>\s*' + re.escape(expr) + r'\s*=', code[:ret.start()]))


def _success_return_before(code: str, original: str, pos: int) -> bool:
    """Whether a `return` before `pos` (outside a lambda) may hand back something other than an error: the
    handler then has a path that ends before `pos`, and what is checked at `pos` is not needed on it."""
    lambdas = _lambda_spans(code)
    for m in _RETURN_RE.finditer(code, 0, pos):
        if any(s <= m.start() < e for s, e in lambdas):
            continue
        if not _is_error_return(code, original, m):
            return True
    return False


def _if_around(code: str, pos: int) -> tuple[int, int, int] | None:
    """(if start, condition open, condition close) of the `if` whose condition holds `pos`, or None."""
    for m in reversed(list(_IF_OPEN_RE.finditer(code, 0, pos))):
        open_at = m.end() - 1
        depth, k = 0, open_at
        while k < len(code):
            if code[k] == "(":
                depth += 1
            elif code[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if open_at < pos < k:
            return m.start(), open_at, k
    return None


def _brace_depth(code: str, pos: int) -> int:
    """Brace depth of `pos` in a function body (its own `{` is depth 1)."""
    return code.count("{", 0, pos) - code.count("}", 0, pos)


def _runs_always(body: str, pos: int) -> bool:
    """Whether the expression at `pos` is evaluated on every path that reaches the end of the function: it sits
    at the top level of the body (brace depth 1), not in an `else if`, not after `&&`, `||` or `?` in its own
    statement or condition, and no `return` that is not an error comes before it."""
    code = _blank_literals(body)
    around = _if_around(code, pos)
    if around is not None:
        start, open_at, _close = around
        if _brace_depth(code, start) != 1 or code[:start].rstrip().endswith("else"):
            return False
        before = code[open_at + 1:pos]
    else:
        if _brace_depth(code, pos) != 1:
            return False
        stmt_start = max(code.rfind(";", 0, pos), code.rfind("{", 0, pos), code.rfind("}", 0, pos)) + 1
        before = code[stmt_start:pos]
    if re.search(r'&&|\|\||\?', before):
        return False
    return not _success_return_before(code, body, pos)


def _guard_runs_always(body: str, pos: int) -> bool:
    """Whether the negated raw read or HasField at `pos` is a guard that fails the call on every path: the
    condition of an `if` at the top level of the body (not an `else if`), joined to other tests by `||` only (an
    `&&` makes it a choice between params, or a guard that only applies sometimes), whose statement returns an
    error, with no `return` that is not an error before it."""
    code = _blank_literals(body)
    around = _if_around(code, pos)
    if around is None:
        return False
    start, open_at, close = around
    if _brace_depth(code, start) != 1 or code[:start].rstrip().endswith("else"):
        return False
    if "&&" in code[open_at:close + 1] or "?" in code[open_at:close + 1]:
        return False
    guarded = _guarded_statement(code, pos)
    if not guarded:
        return False
    stmt_at = code.find(guarded, close)
    returns = list(_RETURN_RE.finditer(code, stmt_at, stmt_at + len(guarded)))
    if not returns or not all(_is_error_return(code, body, r) for r in returns):
        return False
    return not _success_return_before(code, body, start)


def _collect_params(text, body, visited, depth, ordered, required, chains=None, sig="", num_types=None,
                    conditional=False, vec_reads=None):
    """Scan `body` for param reads, then follow `Params`-passing calls (forwarders
    and helpers like ResolveSkeleton) up to `depth` levels deep.

    A negated read makes its param REQUIRED only when a missing value fails the call:
    its guarded statement returns (the error), or the read is not an if-condition at all
    (no way to tell, so it counts as required). A guard that only assigns a fallback --
    `if (!Params->TryGetStringField(TEXT("package_path"), P)) { P = TEXT("/Game/X"); }` --
    is an OPTIONAL param with a default; publishing it as required would make the server
    refuse every call that relies on the default.

    A guarded statement that reads ANOTHER name into the SAME variable is the nested or
    sequential form of an alias chain (`if (!Try(a, V)) { if (!Try(b, V)) Try(c, V); }`):
    one param, several spellings. Like the `&&` chains, the head stays required and the
    rest become optional aliases -- publishing the head AND its first alias as required would
    make a call spell the same thing twice.

    An untyped guard (`!Params->HasField(...)`, `!Params->TryGetField(...)`, RAW_REQUIRED_RE) makes
    its param required only when it runs on every path (_guard_runs_always), and never in a body
    reached through a call that runs only on some paths (`conditional`, see _runs_always).

    `vec_reads` collects how each param's value is read (_value_reads), and `vec_reads[_UNSEEN]` is set
    when a body hands Params to code the scan does not read (_hands_params_unscanned)."""
    for m in REQUIRED_RE.finditer(body):
        name = m.group(1)
        guarded = _guarded_statement(body, m.start())
        if guarded is None:
            required.add(name)
            continue
        target = _read_target(body, m.start())
        spellings = [a.group(1) for a in _READ_INTO_RE.finditer(guarded)
                     if target and a.group(2).strip() == target and a.group(1) != name]
        if spellings:
            # The head of the chain is required only when the handler fails without a value once
            # every spelling has been tried -- inside the chain (its last alias returns the error) or
            # by a later check on the variable; a chain that ends in a default is optional.
            if re.search(r'\breturn\b', guarded) or _missing_value_fails(
                    body, body.find(guarded, m.start()) + len(guarded), target):
                required.add(name)
            if chains is not None:
                head = chains.get(name, name)  # a nested read may itself be an alias already
                chains.setdefault(head, head)
                for alias in spellings:
                    chains[alias] = head
            continue
        if re.search(r'\breturn\b', guarded):
            required.add(name)
    # The untyped guards (RAW_REQUIRED_RE) count only when they run on every path of the handler: in a body
    # reached through a call that itself runs only sometimes (`conditional`), none of them does.
    for m in RAW_REQUIRED_RE.finditer(body):
        if not conditional and _guard_runs_always(body, m.start()):
            required.add(m.group(1))
    if chains is not None:
        for chain_head, alias in _alias_reads(body):
            chains[chain_head] = chain_head  # the head also belongs to its own chain
            chains[alias] = chain_head
    # A param read with TWO getters of DIFFERENT types is POLYMORPHIC, and the type to
    # declare is "any" (-> JsonValue), not whichever read appears first.
    #
    # Keeping the first read's type would make the signature reject valid calls:
    # a property-value param tried with TryGetArrayField -> TryGetObjectField -> scalars has a schema that
    # would say `array` while its description promises "string, number, boolean, JSON object,
    # JSON array or UE text", and a non-array value would fail schema validation before
    # reaching the editor.
    #
    # Widening to "any" cannot break any caller: JsonValue accepts everything the narrow type
    # accepted. HasField does NOT take part -- it states no type, only "exists" -- so it
    # stays as a last-resort filler.
    def _put(pname: str, jt: str) -> None:
        prev = ordered.get(pname)
        if prev is None:
            ordered[pname] = jt
        elif prev != jt and prev != "any":
            ordered[pname] = "any"

    # typed reads, and ReadRequired<Type> (a read of that type), in the order the code makes them
    reads = [(m.start(), m.group(2), JSON_TYPE[m.group(1)]) for m in FIELD_RE.finditer(body)]
    reads += [(m.start(), m.group(2), JSON_TYPE[m.group(1)]) for m in REQUIRED_READ_RE.finditer(body)]
    for _pos, pname, jt in sorted(reads):
        _put(pname, jt)
    for pname in RAW_FIELD_RE.findall(body):          # raw TryGetField -> any JSON type
        _put(pname, "any")
    for pname in VECTOR_HELPER_RE.findall(body):      # ReadJson{Vector,Vector2D,Rotator,LinearColor} -> array
        _put(pname, "array")
    if WORLD_SELECTOR_RE.search(body):                # ResolveWorldSelector -> optional `world`
        _put("world", "string")
    for _type, pname in REQUIRED_READ_RE.findall(body):    # ReadRequired<Type> refuses the call without it
        required.add(pname)
    for pname in HASFIELD_RE.findall(body):           # HasField-only -> assume string
        ordered.setdefault(pname, "string")
    if num_types is not None:                         # the C++ type each number read lands in
        for m in _NUM_READ_RE.finditer(body):
            num_types.setdefault(m.group(3), []).append(_numeric_read_type(sig + body, body, m))
        for m in _REQUIRED_NUM_READ_RE.finditer(body):  # ReadRequiredNumber(Params, TEXT("k"), Var) reads into Var
            num_types.setdefault(m.group(1), []).append(_declared_type(sig + body, m.group(2)))
    if vec_reads is not None:                         # what each param's value is read as (see _vector_shape)
        for pname, how in _value_reads(body):
            vec_reads.setdefault(pname, []).append(how)
    if depth > 0:
        for call in CALLEE_RE.finditer(body):
            callee = call.group(1)
            if callee in visited or callee in _CPP_KEYWORDS:
                continue
            visited.add(callee)
            csig, cbody = _find_function_def(text, callee)
            if cbody:
                _collect_params(text, cbody, visited, depth - 1, ordered, required, chains, csig, num_types,
                                conditional or not _runs_always(body, call.start()), vec_reads)
    # Checked once this body's calls are followed: every function it hands Params to that the scan read is visited.
    if vec_reads is not None and _hands_params_unscanned(text, body, visited):
        vec_reads[_UNSEEN] = ["unseen"]


def _scan_params(text: str, body: str, sig: str = "") -> list[tuple[str, str, bool, str | None, tuple[int, int] | None, str | None]]:
    """Extract (param_name, jsonType, required, chain_key, int_range, vector_shape), following Params-passing calls.

    `chain_key` groups the alternative spellings of one param (the `&&` chain of
    negated reads); it is `None` for everything else. `int_range` is the range of a
    number param whose every read lands in an integer variable (_int_range), else None.
    `vector_shape` is the shape of the one vector reader every read of the param goes
    through (_vector_shape), else None; no param has one when the handler hands Params
    to code the scan does not read."""
    ordered: "OrderedDict[str, str]" = OrderedDict()
    required: set[str] = set()
    chains: dict[str, str] = {}
    num_types: dict[str, list] = {}
    vec_reads: dict[str, list] = {}
    _collect_params(text, body, set(), 2, ordered, required, chains, sig, num_types, vec_reads=vec_reads)
    # An alias in an `&&` chain is still a VALID param (the handler reads it), but it is
    # NOT required: demanding both names at once would be the generator's invention, not
    # the handler's. The HEAD stays required; if it turns out not to be the documented one,
    # the merge with the annotation fixes that later (`_prefer_documented`), which is where
    # the descriptions are.
    required -= {a for a, cab in chains.items() if a != cab}
    shapes = {} if _UNSEEN in vec_reads else {p: _vector_shape(h) for p, h in vec_reads.items()}
    return [(p, jt, p in required, chains.get(p), _int_range(num_types.get(p, [])) if jt == "number" else None,
             shapes.get(p))
            for p, jt in ordered.items()]


def parse_handler_cpp(path: str) -> tuple[list[str], dict[str, list[tuple[str, str, bool, "str | None", "tuple[int, int] | None", "str | None"]]], dict[str, dict], dict[str, int], list]:
    """Returns:
        - list of command strings this handler responds to
        - dict: command_string -> list of (param_name, jsonType, required, chain_key, int_range, vector_shape)
          NOTE: SIX-tuples, straight from _scan_params. Consumers should index, not
          unpack by arity.
        - dict: command_string -> annotation block {description, params, returns}
        - dict: command_string -> duplicate count (>1 = same cmd annotated 2+ times)
        - list of truncated annotations (see find_truncated_annotations)

    The family's route table sends each command to its function (`{ TEXT("cmd"), &HandleXxx }`);
    the params are read inside HandleXxx (and any forwarder/helper it passes Params
    to), so we resolve each cmd -> HandleXxx and scan that function's body, following
    Params-passing calls a couple levels deep.

    Annotations are read from the UNSTRIPPED file (before comment stripping) so we
    keep the `// MCP:<cmd> - ...` blocks the author wrote.
    """
    with open(path, "r", encoding="utf-8") as f:
        raw_text = f.read()
    annotations, annotation_dups = parse_annotations(raw_text)
    truncated = find_truncated_annotations(raw_text)
    text = strip_cpp_comments(raw_text)

    cmds: list[str] = []
    params: dict[str, list[tuple[str, str, bool]]] = {}
    for m in ROUTE_ENTRY_RE.finditer(text):
        cmd, handler = m.group(1), m.group(2)
        if cmd in params:
            continue
        cmds.append(cmd)
        sig, body = _find_function_def(text, handler)
        params[cmd] = _scan_params(text, body, sig)
    return cmds, params, annotations, annotation_dups, truncated

# ---------------------------------------------------------------------------
# Domain mapping
# ---------------------------------------------------------------------------
# Each handler .cpp declares its MCP domain with a `// MCP-DOMAIN: <domain>`
# marker: the domain is sourced from the C++, not a hand-maintained table.
# Scanned at import so any script that imports derive_domain/CLASS_TO_DOMAIN
# sees a populated map.
def _build_class_to_domain() -> dict[tuple[str, str], str]:
    out: dict[tuple[str, str], str] = {}
    for h in glob.glob(HANDLER_GLOB, recursive=True):
        if any(seg in h.split(os.sep) for seg in SKIP_GROUPS):
            continue
        rel = os.path.relpath(h, _paths.COMMANDS_DIR)
        parts = rel.split(os.sep)
        if len(parts) < 2:
            continue
        group, class_name = parts[0], parts[-1].replace(".cpp", "")
        try:
            text = open(h, "r", encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        m = ANNOTATION_DOMAIN_RE.search(text)
        if m:
            out[(group, class_name)] = m.group(1)
    return out

# Populated at import from `// MCP-DOMAIN:` markers (no hand-maintained literal).
CLASS_TO_DOMAIN: dict[tuple[str, str], str] = _build_class_to_domain()

def derive_domain(group: str, class_name: str) -> str:
    """Map a C++ handler class to its MCP domain, read from its `// MCP-DOMAIN:`
    marker. NO silent fallback: a class with `// MCP:` commands but no
    `// MCP-DOMAIN:` aborts the codegen (the domain is the C++'s responsibility)."""
    key = (group, class_name)
    if key in CLASS_TO_DOMAIN:
        return CLASS_TO_DOMAIN[key]
    raise SystemExit(
        f"ERROR: {group}/{class_name}.cpp has // MCP: commands but no "
        f"'// MCP-DOMAIN: <domain>' marker. Declare the domain in the .cpp "
        f"(one marker line per handler file). The domain is never inferred.")

# ---------------------------------------------------------------------------
# Existing-catalog merge
# ---------------------------------------------------------------------------
def load_existing_catalog(path: str) -> dict[str, dict]:
    """Index existing catalog by tool name for fast merge."""
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    by_name: dict[str, dict] = {}
    for d in data.get("domains", []):
        for c in d.get("commands", []):
            by_name[c["name"]] = c
    return by_name

# ---------------------------------------------------------------------------
# Check-mode report
# ---------------------------------------------------------------------------
def describe_drift(want: dict, have_text: str | None) -> list[str]:
    """Say, command by command, how the catalog on disk differs from the generated one.

    Only the report: --check fails on any difference in the text, these lines just
    tell which commands and which of their fields to look at."""
    if have_text is None:
        return [f"{_paths.plugin_relative(OUT_JSON)} does not exist"]
    try:
        have = json.loads(have_text)
    except ValueError as e:
        return [f"{_paths.plugin_relative(OUT_JSON)} is not valid JSON ({e})"]

    def by_name(cat: dict) -> dict[str, tuple[str, dict]]:
        return {c.get("name"): (d.get("domain"), c)
                for d in cat.get("domains", []) for c in d.get("commands", [])}

    def shape(params: list) -> list:
        # Everything but the prose: name, jsonType, required, aliasOf and any field added later.
        return [{k: v for k, v in p.items() if k != "description"} if isinstance(p, dict) else p
                for p in params]

    new, old = by_name(want), by_name(have)
    groups: dict[str, list[str]] = defaultdict(list)
    groups["missing in catalog"] = sorted(new.keys() - old.keys())
    groups["stale in catalog"] = sorted(old.keys() - new.keys(), key=str)
    for name in sorted(new.keys() & old.keys()):
        (new_domain, new_cmd), (old_domain, old_cmd) = new[name], old[name]
        if new_domain != old_domain:
            groups["domain differs"].append(name)
        for field in sorted(new_cmd.keys() | old_cmd.keys()):
            a, b = new_cmd.get(field), old_cmd.get(field)
            if a == b:
                continue
            if field == "params" and isinstance(a, list) and isinstance(b, list) and shape(a) == shape(b):
                groups["param descriptions differ"].append(name)
            elif field == "params":
                groups["params differ (name, type, required, aliasOf, intRange, nameLike or vectorShape)"].append(name)
            else:
                groups[f"{field} differs"].append(name)
    lines = [f"{label} ({len(names)}): {', '.join(map(str, names[:10]))}{' ...' if len(names) > 10 else ''}"
             for label, names in groups.items() if names]
    return lines or ["every command matches; the header, the order or the formatting differs (edited by hand?)"]

# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------
def build(args) -> int:
    # 1) Read each family .cpp: its route table names the commands it answers
    handler_files = sorted(glob.glob(HANDLER_GLOB, recursive=True))
    cmd_to_classes: dict[str, list[str]] = defaultdict(list)   # every family that registers a command
    handler_cpp_index: dict[str, tuple[str, str, list[str], dict]] = {}
    #                                  classname, group, cmds, params
    # Parallel annotations index: {cmd: annotation_dict}, merged across files
    # (first wins, duplicates warned). Also tracks orphan markers (cmd not in
    # any dispatch) and duplicate-block counts.
    cmd_annotations: dict[str, dict] = {}
    annotation_source: dict[str, str] = {}            # cmd -> first file with annotation
    annotation_dup_count: dict[str, int] = {}
    needs_annotation: list[tuple[str, str, str]] = []  # (cmd, group, class) for final report
    # Annotation text the parser silently discards; it is added to all_warnings
    # further down, which does not exist yet at this point of the build.
    truncation_warnings: list[str] = []
    for h in handler_files:
        if any(seg in h.split(os.sep) for seg in SKIP_GROUPS):
            continue
        rel = os.path.relpath(h, _paths.COMMANDS_DIR)
        parts = rel.split(os.sep)
        if len(parts) < 2:
            continue
        group, fname = parts[0], parts[-1]
        class_name = fname.replace(".cpp", "")
        cmds, params, annotations, annotation_dups, truncated_anns = parse_handler_cpp(h)
        for _cmd, _kind, _ln, _how in truncated_anns:
            truncation_warnings.append(
                f"TRUNCATED ANNOTATION: `{_cmd}` {_kind} at {os.path.basename(h)}:{_ln} {_how}"
            )
        handler_cpp_index[class_name] = (group, h, cmds, params)
        for cmd in cmds:
            cmd_to_classes[cmd].append(class_name)
        for acmd, adict in annotations.items():
            if acmd not in cmd_annotations:
                cmd_annotations[acmd] = adict
                annotation_source[acmd] = h
            if acmd in annotation_dups:
                annotation_dup_count[acmd] = annotation_dups[acmd]

    cmd_to_class = {cmd: owners[0] for cmd, owners in cmd_to_classes.items()}

    # Cross-handler fallback: a command whose function only forwards to another
    # family's (through McpCommandFamilies::Run) reads no params of its own.
    # Index params from ANY handler that reads them so the forwarder picks up the
    # real shape.
    cmd_params_any: dict[str, list[tuple[str, str, bool]]] = {}
    for _, (_, _, _, pmap) in handler_cpp_index.items():
        for cmd_str, plist in pmap.items():
            if plist and cmd_str not in cmd_params_any:
                cmd_params_any[cmd_str] = plist

    # 2) Build per-tool entries: the route tables give (string -> family), the
    #    handlers give the params.
    by_domain: dict[str, list[dict]] = defaultdict(list)
    all_warnings: list[str] = list(truncation_warnings)
    dup_strings: dict[str, list[str]] = {
        k: v for k, v in cmd_to_classes.items() if len(v) > 1
    }

    # Old name -> the tools that declare it (`@renamed-from`); checked once every tool is known.
    renamed_from: dict[str, list[str]] = {}

    # Report annotation warnings (orphan markers + duplicates) ONCE, against the
    # final cmd_to_class set so we don't spam for each cpp file.
    all_known_cmds = set(cmd_to_class.keys())
    for acmd in cmd_annotations:
        if acmd not in all_known_cmds:
            src = annotation_source.get(acmd, "?")
            all_warnings.append(
                f"ORPHAN ANNOTATION: `// MCP:{acmd} - ...` in {src} does not match any dispatch"
            )
    for acmd, dup_count in annotation_dup_count.items():
        all_warnings.append(
            f"DUPLICATE ANNOTATION: `// MCP:{acmd}` appears {dup_count + 1} times across files (first wins)"
        )

    for cmd, cls in sorted(cmd_to_class.items()):
        info = handler_cpp_index[cls]
        group = info[0]
        domain = derive_domain(group, cls)
        # Naming warning
        wns = validate_snake(cmd)
        for w in wns:
            all_warnings.append(f"NAMING: {w}  (in {group}/{cls}.cpp)")
        # Param extraction (name, jsonType, required). Prefer the family that
        # routes the cmd; fall back to any handler that reads its params (forwarders).
        pmap = info[3]
        extracted = pmap.get(cmd) or cmd_params_any.get(cmd, [])
        ann = cmd_annotations.get(cmd)
        entry: dict = {
            "name": cmd,
            "description": (ann["description"] if ann and ann.get("description") else ""),
            "params": assemble_params(extracted, ann.get("params", {}) if ann else {}, domain),
            "returns": (ann["returns"] if ann and ann.get("returns") else ""),
        }
        if ann and ann.get("retired"):
            entry["retired"] = dict(ann["retired"])
        if ann and ann.get("core"):
            entry["core"] = True
        if ann and ann.get("pack"):
            entry["pack"] = ann["pack"]
        for old in (ann.get("renamedFrom", []) if ann else []):
            renamed_from.setdefault(old, []).append(cmd)
        by_domain[domain].append(entry)
        # Track cmd that lacks a cpp annotation AND has an empty description —
        # these need human attention.
        if not ann and not entry["description"]:
            needs_annotation.append((cmd, group, cls))

    # Emit a sidecar JSON listing the cmds that lack a cpp annotation AND have
    # no description: the to-do list of annotation work.
    NEEDS_PATH = _paths.report_path("command_catalog_needs_annotation.json")
    needs_payload = {
        "count": len(needs_annotation),
        "commands": [
            {"cmd": c, "group": g, "class": cls}
            for c, g, cls in sorted(needs_annotation)
        ],
    }
    with open(NEEDS_PATH, "w", encoding="utf-8") as f:
        json.dump(needs_payload, f, indent=2, ensure_ascii=False)

    # 5) Report
    total_tools = sum(len(v) for v in by_domain.values())
    print("=" * 78)
    print(f"HANDLERS: {len(handler_cpp_index)} cpp files")
    print(f"TOOLS   : {total_tools} (across {len(by_domain)} domains)")
    print(f"DUPS    : {len(dup_strings)}")
    print(f"WARNINGS: {len(all_warnings)}")
    print("=" * 78)
    print()
    # Per-domain summary
    for d, lst in sorted(by_domain.items()):
        print(f"  [{d}]  {len(lst)} tools")
    print()
    if dup_strings:
        print("--- DUPLICATES (same command string in 2+ handler classes) ---")
        for s, members in sorted(dup_strings.items()):
            print(f"  {s}  <-  {members}")
        print()
    # Naming warnings
    naming_warns = [w for w in all_warnings if w.startswith("NAMING:")]
    if naming_warns:
        print("--- NAMING WARNINGS ---")
        for w in naming_warns:
            print(f"  {w}")
        print()
    orphan_warns = [w for w in all_warnings if w.startswith("ORPHAN ANNOTATION")]
    if orphan_warns:
        print(f"--- ORPHAN ANNOTATIONS ({len(orphan_warns)}) ---")
        for w in orphan_warns:
            print(f"  {w}")
        print()
    dupann_warns = [w for w in all_warnings if w.startswith("DUPLICATE ANNOTATION")]
    if dupann_warns:
        print(f"--- DUPLICATE ANNOTATIONS ({len(dupann_warns)}) ---")
        for w in dupann_warns:
            print(f"  {w}")
        print()
    trunc_warns = [w for w in all_warnings if w.startswith("TRUNCATED ANNOTATION")]
    if trunc_warns:
        print(f"--- TRUNCATED ANNOTATIONS ({len(trunc_warns)}) ---")
        print("  This text does NOT reach the catalog: the parser keeps ONE line per description,")
        print("  @param and @returns, and one @returns per command. Fold the text into that line.")
        print("  (Implementation notes go BELOW the @returns line, at its indentation, where they are")
        print("  ignored harmlessly.)")
        for w in trunc_warns:
            print(f"  {w}")
        print()

    if dup_strings:
        print(f"FAIL: {len(dup_strings)} commands registered by two families; the registry would keep only one.")
        return 2
    shadowed = sorted(META_TOOL_NAMES & set(cmd_to_class))
    if shadowed:
        print(f"FAIL: {shadowed} are meta-tool names the MCP server answers itself; rename the command.")
        return 2
    if args.strict and naming_warns:
        print("STRICT mode: failing because of naming warnings.")
        return 2

    # Renamed and retired tools. An old name must not be a command any more and belongs to one tool; a retired
    # tool points at a LISTED tool, or the reply would send the caller to another name that is not listed either.
    retirement_errors: list[str] = []
    for old, owners in sorted(renamed_from.items()):
        if len(owners) > 1:
            retirement_errors.append(f"`{old}` is the old name of {len(owners)} tools: {', '.join(owners)}")
        if old in all_known_cmds:
            retirement_errors.append(f"`{old}` is declared @renamed-from by {owners[0]} but is still a command")
    all_entries = {e["name"]: e for lst in by_domain.values() for e in lst}
    for name, entry in sorted(all_entries.items()):
        retired = entry.get("retired")
        if not retired:
            continue
        use = all_entries.get(retired["use"])
        if use is None:
            retirement_errors.append(f"`{name}` is @retired for `{retired['use']}`, which is not a command")
        elif use.get("retired"):
            retirement_errors.append(f"`{name}` is @retired for `{retired['use']}`, which is retired too")
    # One verb per operation: a listed tool does not start with a synonym of a verb the catalog uses.
    for name, entry in sorted(all_entries.items()):
        verb = name.split("_", 1)[0]
        if verb in SYNONYM_VERBS and not entry.get("retired") and name not in SYNONYM_VERB_EXCEPTIONS:
            retirement_errors.append(f"`{name}` starts with `{verb}_`: the catalog's verb for that is `{SYNONYM_VERBS[verb]}_`")
    # add_ puts a part in and remove_ takes it out; delete_ is for what create_ makes. A listed delete_<x> next to an
    # add_<x> deletes a part, so its verb is remove_.
    for name, entry in sorted(all_entries.items()):
        if name.startswith("delete_") and not entry.get("retired"):
            added = "add_" + name[len("delete_"):]
            if added in all_entries and not all_entries[added].get("retired"):
                retirement_errors.append(f"`{name}` takes out what `{added}` puts in: its verb is `remove_`")
    # The core list: listed tools only, and few of them.
    core = sorted(n for n, e in all_entries.items() if e.get("core"))
    for name in core:
        if all_entries[name].get("retired"):
            retirement_errors.append(f"`{name}` is @core and @retired: the core list carries listed tools only")
    if len(core) > MAX_CORE_TOOLS:
        retirement_errors.append(f"{len(core)} @core tools, more than {MAX_CORE_TOOLS}: the core list is what every agent pays for")
    print(f"CORE    : {len(core)} tools listed by default")
    # The packs: each one known, holding listed tools that are not core, and none named like a domain.
    pack_tools: dict[str, list[str]] = defaultdict(list)
    for name, entry in sorted(all_entries.items()):
        pack = entry.get("pack")
        if not pack:
            continue
        if pack not in PACKS:
            retirement_errors.append(f"`{name}` is @pack {pack}, which is not in PACKS: {', '.join(PACKS)}")
        if entry.get("core"):
            retirement_errors.append(f"`{name}` is @core and @pack {pack}: a core tool is always listed")
        if entry.get("retired"):
            retirement_errors.append(f"`{name}` is @retired and @pack {pack}: a pack lists listed tools only")
        pack_tools[pack].append(name)
    domain_names = {d.lower() for d in by_domain}
    for pack in PACKS:
        if not pack_tools.get(pack):
            retirement_errors.append(f"pack `{pack}` has no @pack tool")
        if pack.lower() in domain_names:
            retirement_errors.append(f"pack `{pack}` is also a domain name: enable_toolset could not tell them apart")
    print("PACKS   : " + ", ".join(f"{p} ({len(pack_tools.get(p, []))})" for p in PACKS))

    # A listed tool's text sends the agent only to listed tools: an old or retired name in it would be followed.
    gone = set(renamed_from) | {n for n, e in all_entries.items() if e.get("retired")}
    if gone:
        gone_re = re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(sorted(map(re.escape, gone), key=len, reverse=True)) + r")(?![A-Za-z0-9_])")
        for name, entry in sorted(all_entries.items()):
            if entry.get("retired"):
                continue
            texts = [entry.get("description", ""), entry.get("returns", "")]
            texts += [p.get("description", "") for p in entry.get("params", [])]
            named = sorted({m.group(1) for t in texts for m in gone_re.finditer(t or "")})
            if named:
                retirement_errors.append(f"`{name}` names retired or old tool names: {', '.join(named)}")
    if retirement_errors:
        print(f"--- RENAMED / RETIRED ({len(retirement_errors)}) ---")
        for e in retirement_errors:
            print(f"  {e}")
        print("FAIL: renamed or retired tools that a call could not reach.")
        return 2
    if trunc_warns and (args.check or args.strict):
        print(f"FAIL: {len(trunc_warns)} annotations silently lose part of their text.")
        return 2

    # 5a) The MCP tool annotations (readOnlyHint, destructiveHint, idempotentHint,
    # openWorldHint), derived from each command's code and verb by Tools/tool_hints.py.
    entries = {e["name"]: e for lst in by_domain.values() for e in lst}
    hints, hint_errors = tool_hints.classify_catalog(entries)
    for name, entry in entries.items():
        entry["annotations"] = hints[name]
    if hint_errors:
        print(f"--- TOOL HINTS ({len(hint_errors)}) ---")
        for e in hint_errors:
            print(f"  {e}")
        print(f"FAIL: {len(hint_errors)} @hints the code contradicts or the parser does not know.")
        return 2
    print("--- TOOL HINTS: controls ---")
    if not tool_hints.run_controls(hints):
        print("FAIL: a tool_hints control is refuted; the scan cannot be trusted.")
        return 2
    print()

    domains_list = [
        {"domain": d, "commands": lst}
        for d, lst in sorted(by_domain.items())
    ]
    out = {
        "_generated": {
            "by": "codegen_command_catalog.py",
            "source": "the command families' route tables + // MCP: annotations",
            "warning": "AUTO-GENERATED -- DO NOT EDIT BY HAND; run Tools/codegen_command_catalog.py --write",
        },
        "nameLikeRule": name_like_rule(),
        # The packs enable_toolset adds by name; each command names its own (`pack`).
        "packs": [{"name": name, "description": description} for name, description in PACKS.items()],
        "domains": domains_list,
    }
    if renamed_from:
        # Old name -> new name: a call to the old name runs the new tool (FMcpToolCatalog::ResolveCallName).
        out["renamed"] = {old: owners[0] for old, owners in sorted(renamed_from.items())}

    # 5b) Check mode: the catalog on disk must be exactly what --write writes, prose
    # included. The descriptions, @param and @returns ARE the catalog for the agent that
    # reads it, and docs/tools is rendered from this file, so a reworded annotation that
    # was never regenerated would keep publishing the old text with every check green.
    text = json.dumps(out, indent=2, ensure_ascii=False)
    if getattr(args, "check", False):
        have = open(OUT_JSON, encoding="utf-8").read() if os.path.exists(OUT_JSON) else None  # any line ending
        if have != text:
            print("CATALOG DRIFT vs C++ — re-run with --write")
            for line in describe_drift(out, have):
                print(f"  {line}")
            return 1
        print("  catalog in sync with the C++ bridge, prose included")
        return 0

    # 6) Write
    if args.write:
        with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as f:  # LF on every OS, as the repository keeps it
            f.write(text)
        print(f"WROTE   : {OUT_JSON}  (the plugin's MCP server reads it at startup; mcp.http.restart reloads it)")
        # Diff report
        with open(DIFF_TXT, "w", encoding="utf-8") as f:
            f.write(f"Tools total: {total_tools}\n")
            f.write(f"Domains: {len(by_domain)}\n")
            f.write(f"Duplicates: {len(dup_strings)}\n")
            f.write(f"Naming warnings: {len(naming_warns)}\n\n")
            if dup_strings:
                f.write("DUPLICATES:\n")
                for s, members in sorted(dup_strings.items()):
                    f.write(f"  {s}  <-  {members}\n")
                f.write("\n")
            if naming_warns:
                f.write("NAMING WARNINGS:\n")
                for w in naming_warns:
                    f.write(f"  {w}\n")
        print(f"DIFF    : {DIFF_TXT}")
    return 0

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--write", action="store_true", help="regenerate command_catalog.json")
    p.add_argument("--check", action="store_true",
                   help="exit non-zero if the catalog differs in anything, prose included, from what "
                        "--write would write, or if an annotation loses part of its text")
    p.add_argument("--strict", action="store_true", help="fail on naming drift")
    a = p.parse_args()
    return build(a)

if __name__ == "__main__":
    sys.exit(main())
