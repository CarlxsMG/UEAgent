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

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _paths  # noqa: E402

HANDLER_GLOB = os.path.join(_paths.COMMANDS_DIR, "**", "*.cpp")
# The plugin's MCP server (Mcp/McpToolCatalog.cpp) reads this file at startup.
OUT_JSON = _paths.CATALOG_JSON
DIFF_TXT = _paths.report_path("command_catalog_diff.txt")

# The meta-tools of the compact tool list (Mcp/McpMetaTools.cpp): the server answers these
# names itself, so a command registered under one would be unreachable.
META_TOOL_NAMES = {"find_tools", "describe_tool", "call_tool"}

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
# `set_component_property`/`set_blueprint_component_property`'s `property_value`).
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


def assemble_params(extracted: list, ann_params: dict) -> list[dict]:
    """The params a command publishes, built from the C++ alone.

    Every `@param` the annotation documents, in documented order, then the params
    the code reads that no `@param` names, in read order; required ones first
    (stable). Type and required come from the code when it reads the param (the
    regex is anchored to `Params`, so reads off result objects never count), and
    otherwise from the `[type!]` hint (default string/optional). Descriptions come
    from the annotation. Following the documented order keeps the schema stable
    when a handler moves its reads into a helper."""
    code: dict[str, tuple[str, bool]] = {}
    chains: dict[str, str] = {}
    for pname, jt, req, chain_key in extracted:
        if pname not in code:
            code[pname] = (jt, req)
            if chain_key:
                chains[pname] = chain_key
    params: list[dict] = []
    for pname in [*ann_params, *(n for n in code if n not in ann_params)]:
        a = ann_params.get(pname)
        jt, req = code[pname] if pname in code else ((a["type"] or "string"), a["required"])
        params.append({"name": pname, "jsonType": jt, "required": req,
                       "description": (a["desc"] if a else "")})
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
# Vector/rotator helpers (mcp_common::ReadJson{Vector,Vector2D,Rotator})
# read the field as a JSON array via TryGetArrayField and default to zero when it is
# missing, so a param passed to one is an OPTIONAL array. The field name lives at the
# call site (the cross-file helper reads it through its FieldName arg, which the
# extractor cannot follow), so match the call site directly. Anchored to `Params` like
# FIELD_RE so reads off a sub-object are not mistaken for inputs.
VECTOR_HELPER_RE = re.compile(
    r'\bReadJson(?:Vector2D|Vector|Rotator)\s*\(\s*Params\s*,\s*TEXT\("([^"]+)"\)'
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
        if (ANNOTATION_PARAM_RE.match(nxt) or ANNOTATION_RETURNS_RE.match(nxt)
                or ANNOTATION_MARKER_RE.match(nxt) or ANNOTATION_DOMAIN_RE.match(nxt)):
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
                    else "@param" if ANNOTATION_PARAM_RE.match(lines[j]) else None)
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

    Returns: {cmd: {"description": str, "params": {name: {"desc", "type", "required"}}, "returns": str}}.

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
        entry: dict = {"description": desc, "params": {}, "returns": ""}
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
# there Params is the object and the first arg is TEXT(...).)
CALLEE_RE = re.compile(r'\b([A-Za-z_]\w*)\s*\(\s*Params\b')
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
            return _body_from_brace(text, k)
    return ""


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


def _collect_params(text, body, visited, depth, ordered, required, chains=None):
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
    make a call spell the same thing twice."""
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
    if chains is not None:
        for chain_head, alias in _alias_reads(body):
            chains[chain_head] = chain_head  # the head also belongs to its own chain
            chains[alias] = chain_head
    # A param read with TWO getters of DIFFERENT types is POLYMORPHIC, and the type to
    # declare is "any" (-> JsonValue), not whichever read appears first.
    #
    # Keeping the first read's type would make the signature reject valid calls:
    # set_property_value tries TryGetArrayField -> TryGetObjectField -> scalars, so its schema
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
    for pname in VECTOR_HELPER_RE.findall(body):      # ReadJson{Vector,Vector2D,Rotator} -> array
        _put(pname, "array")
    if WORLD_SELECTOR_RE.search(body):                # ResolveWorldSelector -> optional `world`
        _put("world", "string")
    for _type, pname in REQUIRED_READ_RE.findall(body):    # ReadRequired<Type> refuses the call without it
        required.add(pname)
    for pname in HASFIELD_RE.findall(body):           # HasField-only -> assume string
        ordered.setdefault(pname, "string")
    if depth <= 0:
        return
    for callee in CALLEE_RE.findall(body):
        if callee in visited or callee in _CPP_KEYWORDS:
            continue
        visited.add(callee)
        cbody = _find_function_body(text, callee)
        if cbody:
            _collect_params(text, cbody, visited, depth - 1, ordered, required, chains)


def _scan_params(text: str, body: str) -> list[tuple[str, str, bool, str | None]]:
    """Extract (param_name, jsonType, required, chain_key), following Params-passing calls.

    `chain_key` groups the alternative spellings of one param (the `&&` chain of
    negated reads); it is `None` for everything else."""
    ordered: "OrderedDict[str, str]" = OrderedDict()
    required: set[str] = set()
    chains: dict[str, str] = {}
    _collect_params(text, body, set(), 2, ordered, required, chains)
    # An alias in an `&&` chain is still a VALID param (the handler reads it), but it is
    # NOT required: demanding both names at once would be the generator's invention, not
    # the handler's. The HEAD stays required; if it turns out not to be the documented one,
    # the merge with the annotation fixes that later (`_prefer_documented`), which is where
    # the descriptions are.
    required -= {a for a, cab in chains.items() if a != cab}
    return [(p, jt, p in required, chains.get(p)) for p, jt in ordered.items()]


def parse_handler_cpp(path: str) -> tuple[list[str], dict[str, list[tuple[str, str, bool, "str | None"]]], dict[str, dict], dict[str, int], list]:
    """Returns:
        - list of command strings this handler responds to
        - dict: command_string -> list of (param_name, jsonType, required, chain_key)
          NOTE: FOUR-tuples, straight from _scan_params. Consumers should index, not
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
        params[cmd] = _scan_params(text, _find_function_body(text, handler))
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
                groups["params differ (name, type, required or aliasOf)"].append(name)
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
            "params": assemble_params(extracted, ann.get("params", {}) if ann else {}),
            "returns": (ann["returns"] if ann and ann.get("returns") else ""),
        }
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
    if trunc_warns and (args.check or args.strict):
        print(f"FAIL: {len(trunc_warns)} annotations silently lose part of their text.")
        return 2

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
        "domains": domains_list,
    }

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
        with open(OUT_JSON, "w", encoding="utf-8") as f:
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
