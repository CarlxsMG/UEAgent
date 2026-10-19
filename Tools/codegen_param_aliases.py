"""codegen_param_aliases.py — emits the per-command PARAM ALIAS table for the plugin.

Why this exists
---------------
A call that spells a parameter the way a SIBLING tool does loses a round-trip. The
cause is not "hard names", it is vocabulary dispersion across sibling tools, which
the catalog itself shows (counted over command_catalog.json):

    "the path of a blueprint"   -> 6 different names:
        blueprint_name (104 cmds), asset_path (71), anim_blueprint_path (25),
        path (22), object_path (21), blueprint_path (10)
    "the arguments of a call"   -> 3 names that callers reach for: `parameters` (call_function),
        `params` and `args_json` (the spellings of the tools 2.0 folded into it)
    "the state machine"         -> machine_name (12) vs state_machine_name (3)
    "the source state"          -> source_state (AnimBP, 6) vs from_state (StateTree, 1)

Note the winner for "path of a blueprint" is `blueprint_name`, which does not say
"path" although it accepts one — so writing `blueprint_path`, the logical name,
fails on 104 commands.

NOTE: what a wrong name costs. The arguments reach the dispatcher as sent: a name that
is neither canonical nor an alias is not read by the handler, and the dispatcher
REPORTS it in data.unknown_params instead of failing the call. This table turns the
commonest of those misses (the "logical" spelling of a sibling tool) into the
canonical name, and REPORTS that too (data.aliased_params) rather than applying it
quietly.

Two families, derived differently
---------------------------------
1. SUFFIX (derived mechanically). The caller writes the tail of the name:
   `pattern` for `search_pattern`, `machine_name` for `state_machine_name`. The rule is
   strict: the canonical must be exactly `<something>_<alias>` and there must be
   EXACTLY ONE such param on that command; 0 or 2+ candidates emit nothing.

   The strictness matters. Deriving from a loose suffix (`_state`
   for the alias `source_state`) produces nonsense — `show_notification`'s
   `completion_state` would "answer" to `source_state`. Requiring the WHOLE alias as
   the suffix drops those to zero.

2. EXPLICIT (hand-written, with evidence). True synonyms that share no suffix:
   `source_state`<->`from_state`, and the `params`/`parameters`/`args_json` trio.
   These cannot be derived and are listed below one by one.

Both families are emitted as ONE flat, pre-resolved command -> [(alias, canonical)]
table, so the dispatcher does no suffix matching at runtime.

Safety
------
The generated table is only half the guarantee; the other half is the dispatcher's
four conditions (see McpParamAliases.gen.h). The one that makes bidirectional pairs
safe is "the command does NOT know the alias": `set_property_access_path` is the only
command that declares both `path` and `blueprint_name`, and it excludes itself.

Coverage of the main pairs in the catalog (applicable / commands that declare BOTH):
    blueprint_path -> blueprint_name    104 / 0
    machine_name   -> state_machine_name  3 / 0
    parameters     -> params              2 / 0
    args_json      -> parameters          2 / 0

ORDER MATTERS: this reads `command_catalog.json`, which is generated from the C++.
Run `codegen_command_catalog.py --write` FIRST.

Usage:
  python Tools/codegen_param_aliases.py            # report only (default)
  python Tools/codegen_param_aliases.py --write    # regenerate the .h/.cpp
  python Tools/codegen_param_aliases.py --check    # exit non-zero if the files drifted
"""

from __future__ import annotations
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _paths  # noqa: E402

CATALOG_JSON = _paths.CATALOG_JSON
PLUGIN_SRC = _paths.SOURCE_DIR
HEADER_NAME = "McpParamAliases.gen.h"
SOURCE_NAME = "McpParamAliases.gen.cpp"
OUT_H = os.path.join(PLUGIN_SRC, "Public", HEADER_NAME)
OUT_CPP = os.path.join(PLUGIN_SRC, "Private", SOURCE_NAME)

GENERATOR = "Tools/codegen_param_aliases.py"
SOURCE = "Resources/command_catalog.json"

# ---------------------------------------------------------------------------
# FAMILY 1 — suffix aliases, derived from the catalog.
#
# For alias A on a command that does NOT declare A: the canonical is the ONLY param
# ending in "_A". Two or more candidates => ambiguous => emit nothing for that
# command (a guess there would be worse than the round-trip it saves).
#
# The list is deliberately short: every entry is a name callers actually write, or the
# obvious tail of a dispersed family. Adding one is
# cheap; adding one that is not really a synonym is a silent mistranslation.
# ---------------------------------------------------------------------------
SUFFIX_ALIASES: list[tuple[str, str]] = [
    ("machine_name", "add_anim_transition says machine_name and its siblings state_machine_name"),
    ("pattern", "search_assets_by_pattern calls it search_pattern"),
]

# PRUNED, written down so they are not re-added: path, name, index, tag, property,
# class_name. The suffix rule would derive thousands of pairs from them (`path` and
# `name` alone would be most of the table), and callers do not send them in place of the
# longer names: every pair would be pure schema cost -- and the agent reads the schema
# every time it loads the tool.
#
# Slipping in through the suffix rule is easy; paying for itself has to be shown. Re-add
# one only with evidence that callers send it.

# ---------------------------------------------------------------------------
# FAMILY 2 — explicit synonyms. NOT derivable: they share no suffix.
#
# (alias, canonical, evidence). Emitted for every command that declares `canonical`
# and does not declare `alias`; the dispatcher's conditions do the rest.
# ---------------------------------------------------------------------------
EXPLICIT_ALIASES: list[tuple[str, str, str]] = [
    ("source_state", "from_state",
     "AnimBP tools (add_anim_transition) say source_state; StateTree tools "
     "(add_state_transition) say from_state"),
    ("from_state", "source_state",
     "the same pair in the other direction"),
    ("target_state", "to_state",
     "same split as source_state/from_state, on the other end of the transition"),
    ("to_state", "target_state",
     "the same pair in the other direction"),
    ("blueprint_path", "blueprint_name",
     "104 commands accept an asset PATH in a param called blueprint_NAME; "
     "blueprint_path is the name a caller reaches for and it exists on only 10"),
    ("parameters", "params",
     "call_function takes `parameters`; `params` and `args_json` are the spellings of "
     "the call tools 2.0 folded into it: three names for the same dict"),
    ("params", "parameters",
     "the same trio in the other direction"),
    ("args_json", "parameters",
     "the old call_object_method spelling of the same dict"),
    ("args_json", "params",
     "the old call_object_method spelling, pointed at the `params` spelling"),
    ("parameters", "args_json",
     "the same trio in the other direction"),
    ("params", "args_json",
     "the same trio in the other direction"),
    ("args", "params",
     "the bare spelling nobody uses but everybody tries"),
    ("args", "parameters",
     "same, for the call_function side"),
    # Direction matters here:
    # find_blueprint_node_references is the tool that CALLS it `symbol`, so aliasing
    # symbol->function_name would do nothing for it (condition 4 excludes it) and would
    # teach 20 unrelated commands to answer to `symbol`. The useful rewrite is the
    # inverse: a caller who writes the ordinary `function_name` at that tool.
    ("function_name", "symbol",
     "find_blueprint_node_references calls the thing being searched for `symbol`; "
     "every sibling tool calls it function_name"),
    # Each entry below is a spelling callers send to a tool that names the thing otherwise.
    # Do NOT add blueprint_name->symbol. It is SEMANTICALLY WRONG: when
    # find_blueprint_node_references is called with {blueprint_name, function_name},
    # blueprint_name is not what is searched for -- it is WHERE
    # to search, i.e. `owner_blueprint`, which that tool already declares. Translating it
    # to `symbol` would search for the blueprint's name as if it were the symbol: a
    # plausible, wrong result, which is exactly what an alias cannot afford. It stays
    # without an alias: the C++ error naming the parameter beats an invented translation.
    ("search_term", "symbol", "the generic name callers give to a search term"),
    ("code", "command",
     "execute_python calls `command` what everybody else calls code"),
    ("script", "command", "same family as code->command"),
    ("asset_path", "path",
     "save_asset calls `path` what 71 commands call asset_path"),
    ("name_pattern", "pattern",
     "list_enums/list_modules/list_structs say name_pattern and their neighbours say pattern"),
    ("enum_path", "enum_name", "get_enum_info, called with the Asset Registry object path"),
    ("tag_query", "query_tag", "query_gameplay_tags calls it query_tag"),
]

WRAP_COLUMN = 100
IDENT_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def load_catalog(path: str) -> dict[str, list[str]]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    out: dict[str, list[str]] = {}
    for domain in data.get("domains", []):
        for cmd in domain.get("commands", []):
            name = cmd.get("name")
            if not name:
                continue
            names = sorted({p["name"] for p in cmd.get("params", []) if p.get("name")})
            if name in out:
                out[name] = sorted(set(out[name]) | set(names))
            else:
                out[name] = names
    return dict(sorted(out.items()))


def build_table(catalog: dict[str, list[str]]) -> tuple[dict[str, list[tuple[str, str]]], dict]:
    """command -> sorted [(alias, canonical)], plus stats for the report."""
    table: dict[str, list[tuple[str, str]]] = {}
    stats = {
        "suffix_applied": 0,
        "suffix_ambiguous": 0,
        "explicit_applied": 0,
        "collisions_skipped": 0,
        "per_alias": {},
    }

    def add(cmd: str, alias: str, canonical: str) -> bool:
        params = set(catalog[cmd])
        # Condition mirrored in C++ (belt and braces): never emit a row whose alias the
        # command actually declares — that command means something else by that name.
        if alias in params or canonical not in params:
            return False
        pairs = table.setdefault(cmd, [])
        if any(a == alias for a, _ in pairs):  # first rule wins, deterministically
            return False
        pairs.append((alias, canonical))
        stats["per_alias"][alias] = stats["per_alias"].get(alias, 0) + 1
        return True

    # No generic X_path <-> X_name family, on purpose: "every X_path param also accepts X_name,
    # and vice versa" would roughly double the table (the agent reads the schema on EVERY tool
    # load) and produce nonsense pairs: add_graph_node accepting `enum_name`, `variable_path` and
    # `macro_graph_path` as if they were the same thing. Only the concrete case below stays.
    #
    # The caller writes the generic `blueprint_name` on a tool that calls the same thing
    # `anim_blueprint_path` or `widget_bp_path` (add_anim_state, get_widget_blueprint_tree).
    for cmd, params in catalog.items():
        if "blueprint_name" in params:
            continue
        cand = [p for p in params if p.endswith("blueprint_path") or p.endswith("bp_path")]
        if len(cand) == 1:
            if add(cmd, "blueprint_name", cand[0]):
                stats["suffix_applied"] += 1

    for cmd, params in catalog.items():
        pset = set(params)
        for alias, _why in SUFFIX_ALIASES:
            if alias in pset:
                stats["collisions_skipped"] += 1
                continue
            suffix = "_" + alias
            candidates = [p for p in params if p.endswith(suffix)]
            if len(candidates) == 1:
                if add(cmd, alias, candidates[0]):
                    stats["suffix_applied"] += 1
            elif len(candidates) > 1:
                stats["suffix_ambiguous"] += 1

    for alias, canonical, _why in EXPLICIT_ALIASES:
        for cmd in catalog:
            if add(cmd, alias, canonical):
                stats["explicit_applied"] += 1

    return {c: sorted(p) for c, p in sorted(table.items())}, stats


def validate(table: dict[str, list[tuple[str, str]]]) -> list[str]:
    problems: list[str] = []

    def bad(s: str) -> bool:
        return (not s) or (not s.isascii()) or any(c not in IDENT_OK for c in s)

    for cmd, pairs in table.items():
        if bad(cmd):
            problems.append(f"command name is not a plain identifier: {cmd!r}")
        seen: set[str] = set()
        for alias, canonical in pairs:
            if bad(alias) or bad(canonical):
                problems.append(f"alias pair is not a plain identifier: {cmd}.{alias!r}->{canonical!r}")
            if alias == canonical:
                problems.append(f"self-alias: {cmd}.{alias}")
            if alias in seen:
                problems.append(f"alias declared twice on the same command: {cmd}.{alias}")
            seen.add(alias)
    return problems


def banner(kind: str) -> str:
    return (
        f"// AUTO-GENERATED by {GENERATOR} from {SOURCE} -- DO NOT EDIT BY HAND.\n"
        f"// To regenerate, IN THIS ORDER (the table comes from the catalog, the catalog from the C++):\n"
        f"//   python Tools/codegen_command_catalog.py --write\n"
        f"//   python {GENERATOR} --write\n"
        f"//\n"
        f"// {kind}"
    )


def render_header(table: dict[str, list[tuple[str, str]]]) -> str:
    n_cmds = len(table)
    n_pairs = sum(len(v) for v in table.values())
    L: list[str] = []
    a = L.append
    a(banner("Public API of the param-alias table (declarations only; the data lives in the .cpp)."))
    a("//")
    a("// The problem it solves: the catalog names \"the path of a blueprint\" in 6 different")
    a("// ways (blueprint_name in 104 commands, asset_path in 71, path in 22, ...) and the")
    a("// argument dictionary of four SIBLING tools in 3 ways (parameters / params /")
    a("// args_json), and a misspelled name costs a round-trip.")
    a("//")
    a("// It is NOT silent and does NOT guess: the dispatcher translates, REPORTS it in")
    a("// data.aliased_params, and only when all FOUR conditions hold:")
    a("//   1. the caller sent the alias")
    a("//   2. and did NOT send the canonical -> a caller's value is never overwritten")
    a("//   3. the command knows the canonical")
    a("//   4. and does NOT know the alias    -> a command that gives that name ANOTHER")
    a("//                                        meaning excludes itself (set_property_access_path")
    a("//                                        is the only one declaring `path` and `blueprint_name`)")
    a("//")
    a("// NOTE: a name that is neither canonical nor an alias does NOT fail the call: the handler")
    a("// does not read it and the dispatcher REPORTS it in data.unknown_params. This table turns")
    a("// the most common miss (the \"logical\" spelling of a sibling tool) into the canonical name.")
    a("//")
    a(f"// Coverage: {n_cmds} commands / {n_pairs} alias->canonical pairs.")
    a("#pragma once")
    a("")
    a("#include \"CoreMinimal.h\"")
    a("")
    a("namespace McpParamAliases")
    a("{")
    a("    /** One alias -> canonical rewrite that applies to a given command. */")
    a("    struct FAlias")
    a("    {")
    a("        FName Alias;")
    a("        FName Canonical;")
    a("    };")
    a("")
    a("    /** Number of commands that have at least one alias. */")
    a("    UEAGENT_API int32 NumCommands();")
    a("")
    a("    /**")
    a("     * The alias rules of CommandName, or an EMPTY view when it has none.")
    a("     * Never null; iterate it directly. Keys are FName, so lookups are")
    a("     * case-insensitive, matching FJsonObject's own field lookup.")
    a("     */")
    a("    UEAGENT_API TConstArrayView<FAlias> For(FName CommandName);")
    a("}")
    a("")
    return "\n".join(L)


def render_cpp(table: dict[str, list[tuple[str, str]]]) -> str:
    n_cmds = len(table)
    n_pairs = sum(len(v) for v in table.values())
    L: list[str] = []
    a = L.append
    a(banner("Data + lookups of the param-alias table."))
    a("#include \"" + HEADER_NAME + "\"")
    a("")
    a("namespace McpParamAliases")
    a("{")
    a("// Named (NOT anonymous) namespace on purpose: this module builds with")
    a("// bUseUnity = true and anonymous-namespace helpers have collided across unity")
    a("// blobs in this plugin before (see bUseUnity in UEAgent.Build.cs).")
    a("namespace Generated")
    a("{")
    a("")
    a("// ONE flat array of string literals: `cmd, alias1, canonical1, ..., nullptr` per")
    a("// command, plus a final nullptr sentinel. Walked ONCE into a function-local static")
    a("// map. Deliberately not N Map.Add() statements -- far smaller object code.")
    a("static const TCHAR* const GRows[] =")
    a("{")
    for cmd, pairs in table.items():
        a(f"    TEXT(\"{cmd}\"),")
        line = "       "
        for alias, canonical in pairs:
            chunk = f" TEXT(\"{alias}\"), TEXT(\"{canonical}\"),"
            if len(line) + len(chunk) > WRAP_COLUMN:
                a(line)
                line = "       "
            line += chunk
        if line.strip():
            a(line)
        a("        nullptr,")
    a("    nullptr, // end-of-table sentinel")
    a("};")
    a("")
    a("static const TMap<FName, TArray<FAlias>>& GetMap()")
    a("{")
    a("    static const TMap<FName, TArray<FAlias>> Map = []")
    a("    {")
    a("        TMap<FName, TArray<FAlias>> Out;")
    a(f"        Out.Reserve({n_cmds});")
    a("        const TCHAR* const* Cursor = GRows;")
    a("        while (*Cursor)")
    a("        {")
    a("            const FName Command(*Cursor++);")
    a("            TArray<FAlias>& Pairs = Out.Add(Command);")
    a("            while (*Cursor)")
    a("            {")
    a("                const FName Alias(*Cursor++);")
    a("                checkf(*Cursor, TEXT(\"McpParamAliases: alias without canonical (table is malformed)\"));")
    a("                Pairs.Add(FAlias{ Alias, FName(*Cursor++) });")
    a("            }")
    a("            ++Cursor; // step over the per-command nullptr")
    a("        }")
    a("        return Out;")
    a("    }();")
    a("    return Map;")
    a("}")
    a("")
    a("} // namespace Generated")
    a("")
    a("int32 NumCommands()")
    a("{")
    a("    return Generated::GetMap().Num();")
    a("}")
    a("")
    a("TConstArrayView<FAlias> For(FName CommandName)")
    a("{")
    a("    if (const TArray<FAlias>* Found = Generated::GetMap().Find(CommandName))")
    a("    {")
    a("        return TConstArrayView<FAlias>(*Found);")
    a("    }")
    a("    return TConstArrayView<FAlias>();")
    a("}")
    a("")
    a("} // namespace McpParamAliases")
    a("")
    a(f"// {n_cmds} commands / {n_pairs} pairs.")
    return "\n".join(L)


def read_if_exists(path: str) -> str | None:
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--write", action="store_true", help=f"regenerate {HEADER_NAME} / {SOURCE_NAME}")
    p.add_argument("--check", action="store_true", help="exit non-zero if the generated files drifted")
    a = p.parse_args()

    if not os.path.exists(CATALOG_JSON):
        print(f"ERROR: catalog not found: {CATALOG_JSON}")
        return 3
    catalog = load_catalog(CATALOG_JSON)
    table, stats = build_table(catalog)
    problems = validate(table)
    if problems:
        print(f"ERROR: {len(problems)} unusable alias row(s); refusing to emit C++")
        for w in problems[:20]:
            print(f"  {w}")
        return 4

    header = render_header(table)
    cpp = render_cpp(table)
    n_pairs = sum(len(v) for v in table.values())

    print("=" * 78)
    print(f"CATALOG  : {CATALOG_JSON}  ({len(catalog)} commands)")
    print(f"ALIASES  : {n_pairs} pairs over {len(table)} commands")
    print(f"  suffix   : {stats['suffix_applied']} applied, {stats['suffix_ambiguous']} skipped as AMBIGUOUS")
    print(f"  explicit : {stats['explicit_applied']} applied")
    print(f"  collision: {stats['collisions_skipped']} skipped (command declares the alias itself)")
    for alias, n in sorted(stats["per_alias"].items(), key=lambda kv: -kv[1]):
        print(f"      {alias:22} {n:5}")
    print(f"HEADER   : {OUT_H}  ({header.count(chr(10))} lines)")
    print(f"SOURCE   : {OUT_CPP}  ({cpp.count(chr(10))} lines)")
    print("=" * 78)

    if a.check:
        drift = False
        for path, want in ((OUT_H, header), (OUT_CPP, cpp)):
            have = read_if_exists(path)
            if have is None:
                print(f"MISSING : {path} — re-run with --write")
                drift = True
            elif have != want:
                print(f"DRIFT   : {path} differs from the catalog — re-run with --write")
                drift = True
        if drift:
            return 1
        print("  generated alias table in sync with the catalog")
        return 0

    if a.write:
        write_text(OUT_H, header)
        write_text(OUT_CPP, cpp)
        print(f"WROTE   : {OUT_H}")
        print(f"WROTE   : {OUT_CPP}")
    else:
        print()
        print("(dry-run; pass --write to apply)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
