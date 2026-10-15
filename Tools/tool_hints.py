"""tool_hints.py -- the MCP tool annotations (readOnlyHint, destructiveHint, idempotentHint,
openWorldHint) of every command, derived from the C++.

Why this exists
---------------
A client reads a tool's annotations to decide how much care a call needs: whether it can run
without asking (read-only), whether it may lose work (destructive), whether a retry is safe
(idempotent), and whether it reaches outside the editor (open world). Without them every tool
looks like the protocol's defaults -- a destructive, non-idempotent, open-world write -- and a
reader cannot tell get_blueprint_info from delete_asset without reading the prose.

Where the verdict comes from -- and why it is NOT hand-written
-------------------------------------------------------------
A hand-kept list rots the moment someone adds a command, so the verdict is derived from the C++,
with the attribution method of Tools/codegen_pie_safety.py:

  cmd -> handler     `{ TEXT("x"), &HandleX }` in the family's route table
  handler -> code    the handler's body, the same-file helpers it calls (one level) and the
                     mcp_common helpers any of those call
  code -> evidence   a write marker (Modify, MarkPackageDirty, a save, a transaction, spawning
                     or destroying an actor, creating or deleting an asset, writing a file or
                     a config, compiling a Blueprint...) outside comments and string literals

and the command's verb (the first word of its name, or the second after a domain prefix such
as `interchange_`):

  readOnlyHint     the verb only reads (get, list, find, inspect...) AND the code shows no write
                   marker. A reader that writes -- an audit that saves a report, a check that
                   compiles -- is NOT read-only, whatever its name says.
  destructiveHint  false only when the verb adds something new (add, create, spawn, duplicate...)
                   and nothing in the code or the description deletes, replaces or overwrites;
                   true for every other write.
  idempotentHint   true for a reader, for a verb that sets a state (set, configure, enable,
                   disable, clear, reset), and for a command whose description says it is
                   idempotent; false otherwise.
  openWorldHint    true only when the code talks to the network or runs arbitrary code
                   (execute_python): every other command stays inside the editor and its project.

The defaults err on the side of care: a wrong `readOnlyHint: true` lets a client run a write
without asking, while a wrong `false` only costs a confirmation.

The annotation overrides it
---------------------------
When the name misleads the verb rule (`frame_timing` reads, `foliage_type_exists` reads), the
command's `// MCP:` block says so on its own line:

    //   @hints read-only
    //   @hints additive idempotent

Tokens: read-only, destructive, additive, idempotent, not-idempotent, open-world, closed-world.
`additive` or `destructive` makes a command a write whatever its verb says.
`read-only` on a command whose code shows a write marker is an ERROR, not an override: the
evidence is printed and the catalog is not written.

Controls (they gate the catalog codegen)
----------------------------------------
POSITIVE read-only: get_blueprint_info, list_meta_sounds, get_graph_nodes, find_blueprint_node_references.
NOT read-only: delete_asset, compile_blueprint, get_compile_errors (it compiles),
audit_blueprint_health (it compiles and saves), set_actor_label.
Destructive: delete_asset, remove_widget, set_actor_label. Additive: add_graph_node, create_blueprint.
A scan that cannot reproduce those is refuted.

Usage:
  python Tools/tool_hints.py              # summary
  python Tools/tool_hints.py --list       # every command with its hints and evidence
  python Tools/tool_hints.py --controls   # run the controls and stop
  python Tools/tool_hints.py --why <cmd>  # one command's verdict with its evidence
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
import _paths  # noqa: E402
import codegen_pie_safety as ps  # noqa: E402

CMDS_DIR = _paths.COMMANDS_DIR_P

# ---------------------------------------------------------------------------
# Write markers: call-shaped, so a variable named TotalDuplicateAssets is not a write.
# ---------------------------------------------------------------------------
WRITE_MARKERS = [(re.compile(p), tag) for p, tag in [
    (r"(?:->|\.)\s*Modify\s*\(", "modifies an object"),
    (r"\b(?:MarkPackageDirty|SetDirtyFlag)\s*\(", "dirties a package"),
    (r"\b(?:SavePackage|SaveAsset|SaveLoadedAsset|SaveLoadedAssets|SaveDirtyPackages|SaveMap|SaveCurrentLevel"
     r"|SaveDirtyPackage|SavePackages)\s*\(|UEditorLoadingAndSavingUtils::", "saves"),
    (r"\bFScopedTransaction\b", "opens a transaction"),
    (r"\b(?:SpawnActor|SpawnActorDeferred|DestroyActor|K2_DestroyActor)\b|->\s*Destroy\s*\(", "spawns or destroys an actor"),
    (r"\b(?:DeleteAssets|DeleteObjects|ForceDeleteObjects|DeleteAsset|DeleteLoadedAsset|DeleteDirectory)\s*\(|ObjectTools::Delete",
     "deletes"),
    (r"\b(?:CreateAsset|CreatePackage|DuplicateAsset|RenameAssets|RenameAsset|ImportAssets|ImportAssetTasks"
     r"|ImportAssetsAutomated|ImportAsset|Reimport|ReimportAsync|ConsolidateAssets)\s*\(", "creates, renames or imports an asset"),
    (r"\b(?:PostEditChange|PostEditChangeProperty|PostEditChangeChainProperty|SetEditorProperty)\s*\(", "changes a property"),
    (r"\b(?:ExecuteConsoleCommand|ExecCmds)\s*\(|\b(?:GEngine|GEditor)\s*->\s*Exec\s*\(", "runs a console command"),
    (r"FFileHelper::Save|IFileManager::Get\(\)\s*\.\s*(?:Delete|Move|Copy|MakeDirectory)\s*\("
     r"|\b(?:DeleteFile|DeleteDirectoryRecursively|CreateDirectoryTree|CopyFile|MoveFile)\s*\(", "writes a file"),
    (r"\b(?:CompileBlueprint|CompileSynchronously|MarkBlueprintAsStructurallyModified|MarkBlueprintAsModified)\s*\(",
     "compiles a Blueprint"),
    (r"\b(?:SelectActor|SelectNone|SelectComponent)\s*\(", "changes the selection"),
    (r"\b(?:RequestPlaySession|RequestEndPlayMap|EndPlayMap)\s*\(", "starts or stops PIE"),
    (r"\b(?:SetActorLocation|SetActorRotation|SetActorTransform|SetActorScale3D|SetActorLabel|SetWorldTransform"
     r"|SetRelativeLocation|SetRelativeRotation|SetRelativeTransform|SetWorldLocation)\s*\(", "moves or renames an actor"),
    (r"\bGConfig\s*->\s*Set\w*\s*\(|\b(?:SaveConfig|TryUpdateDefaultConfigFile|UpdateDefaultConfigFile|UpdateSinglePropertyInConfigFile)\s*\(",
     "writes a config"),
    (r"\b(?:LoadMap|NewMap|OpenMap)\s*\(", "opens a level"),
]]
NETWORK_MARKER = re.compile(r"\bFHttpModule\b|\b(?:LaunchURL)\s*\(")

# ---------------------------------------------------------------------------
# Verbs.
# ---------------------------------------------------------------------------
# A domain prefix comes before the verb: interchange_get_pipelines, metasound_add_interface.
DOMAIN_PREFIXES = {"interchange", "metasound", "batch", "bulk"}
READ_VERBS = {
    "get", "list", "find", "inspect", "diff", "compare", "audit", "validate", "check", "verify",
    "is", "has", "can", "does", "evaluate", "read", "scan", "search", "query", "count", "poll",
    "analyze", "resolve", "lint", "discover", "detect", "diagnose", "dump", "suggest", "profile",
    "test", "trace",
}
ADDITIVE_VERBS = {
    "add", "create", "spawn", "duplicate", "copy", "generate", "scaffold", "register", "grant",
    "insert", "define", "subscribe", "push", "bookmark", "append", "place",
}
IDEMPOTENT_VERBS = {"set", "configure", "enable", "disable", "clear", "reset"}
# Run arbitrary code: anything can happen, the network included.
ARBITRARY = {"execute_python", "execute_python_file"}

# Wording that makes an additive verb destructive: it replaces what was there. A sibling tool's
# name (`remove_anim_sync_marker is the inverse`) and a negated mention (`refuses instead of
# overwriting`) do not count.
REPLACES_RE = re.compile(r"\b(?:overwrit\w*|replac\w*|delet\w*|remov\w*|destroy\w*|clears|cleared|clearing|discard\w*"
                         r"|upsert\w*|reset\w*)\b", re.I)
NEGATION_RE = re.compile(r"\b(?:not|never|without|refus\w*|instead of|no|nothing)\b[^.;]{0,30}$", re.I)
REPLACE_PARAMS = {"overwrite", "replace", "replace_existing", "force_overwrite", "overwrite_existing", "clear_existing"}
IDEMPOTENT_RE = re.compile(r"(?<!not )(?<!NOT )\bidempotent\b", re.I)

HINT_TOKENS = {"read-only", "destructive", "additive", "idempotent", "not-idempotent", "open-world", "closed-world"}
# `//   @hints <tokens>` inside a command's `// MCP:` block.
HINTS_LINE_RE = re.compile(r"^//\s{3,}@hints\s+(.+)$")
# The block's marker, as codegen_command_catalog.ANNOTATION_MARKER_RE reads it.
MARKER_RE = re.compile(r"^//\s*MCP:(\S+)\s+-\s+(.+)$")

POSITIVE_READ_ONLY = ["get_blueprint_info", "list_meta_sounds", "get_graph_nodes", "find_blueprint_node_references"]
NOT_READ_ONLY = ["delete_asset", "compile_blueprint", "get_compile_errors", "audit_blueprint_health", "set_actor_label"]
DESTRUCTIVE = ["delete_asset", "remove_widget", "set_actor_label"]
ADDITIVE = ["add_graph_node", "create_blueprint"]

STRING_RE = re.compile(r'"(?:\\.|[^"\\\n])*"')


def code_only(line: str) -> str:
    """The line without string literals and without a trailing // comment."""
    line = STRING_RE.sub('""', line)
    cut = line.find("//")
    return line if cut < 0 else line[:cut]


def replaces_something(description: str) -> bool:
    """Whether the description says the call deletes, replaces or overwrites what was there."""
    for m in REPLACES_RE.finditer(description):
        if "_" in m.group(0) or NEGATION_RE.search(description[:m.start()]):
            continue
        return True
    return False


def verb_of(cmd: str) -> str:
    words = cmd.split("_")
    if words[0] in DOMAIN_PREFIXES and len(words) > 1:
        return words[1]
    return words[0]


def scan_evidence() -> dict[str, list[tuple[str, str]]]:
    """{cmd: [(what, 'file function:line'), ...]} -- every write marker in the code a command runs."""
    shared_lines = (CMDS_DIR / ps.SHARED_REL).read_text(encoding="utf-8", errors="replace").splitlines()
    shared = ps.function_bodies(shared_lines)
    out: dict[str, list[tuple[str, str]]] = {}
    for cpp in sorted(CMDS_DIR.rglob("*.cpp")):
        lines = cpp.read_text(encoding="utf-8", errors="replace").splitlines()
        bodies = ps.function_bodies(lines)
        rel = cpp.relative_to(CMDS_DIR).as_posix()
        for ln in lines:
            rm = ps.ROUTE_RE.search(ln)
            if not rm:
                continue
            cmd, handler = rm.group(1), rm.group(2)
            if handler not in bodies:
                out[cmd] = [("its handler was not found", f"{rel} {handler}")]
                continue
            funcs = [(rel, handler, bodies[handler])]
            text = "\n".join(bodies[handler][1])
            for callee in set(ps.LOCAL_CALL_RE.findall(text)):
                if callee != handler and callee in bodies:
                    funcs.append((rel, callee, bodies[callee]))
            for callee in {n for _, _, (_, body) in funcs for n in ps.SHARED_CALL_RE.findall("\n".join(body))}:
                if callee in shared:
                    funcs.append((ps.SHARED_REL, callee, shared[callee]))
            hits: list[tuple[str, str]] = []
            for frel, fname, (start, body) in funcs:
                for off, raw in enumerate(body):
                    if raw.lstrip().startswith("//"):
                        continue
                    line = code_only(raw)
                    for rx, what in WRITE_MARKERS:
                        if rx.search(line):
                            hits.append((what, f"{frel} {fname}:{start + off}"))
                    if NETWORK_MARKER.search(line):
                        hits.append(("talks to the network", f"{frel} {fname}:{start + off}"))
            out.setdefault(cmd, hits)
    return out


def parse_hint_tags(text: str) -> dict[str, list[str]]:
    """{cmd: [tokens]} from the `@hints` lines of the `// MCP:` blocks of one file."""
    out: dict[str, list[str]] = {}
    cmd = None
    for line in text.splitlines():
        m = MARKER_RE.match(line)
        if m:
            cmd = m.group(1)
            continue
        if cmd is None or not line.startswith("//"):
            cmd = None
            continue
        h = HINTS_LINE_RE.match(line)
        if h:
            out.setdefault(cmd, []).extend(h.group(1).split())
    return out


def scan_hint_tags() -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for cpp in sorted(CMDS_DIR.rglob("*.cpp")):
        for cmd, tokens in parse_hint_tags(cpp.read_text(encoding="utf-8", errors="replace")).items():
            out.setdefault(cmd, []).extend(tokens)
    return out


def classify(cmd: str, description: str, param_names: set[str], evidence: list[tuple[str, str]],
             tokens: list[str]) -> tuple[dict, list[str]]:
    """-> (annotations, errors) for one command."""
    errors = [f"{cmd}: unknown @hints token '{t}' (use {', '.join(sorted(HINT_TOKENS))})"
              for t in tokens if t not in HINT_TOKENS]
    writes = [e for e in evidence if e[0] != "talks to the network"]
    network = any(e[0] == "talks to the network" for e in evidence)
    verb = verb_of(cmd)

    read_only = verb in READ_VERBS and not writes and cmd not in ARBITRARY
    if "read-only" in tokens:
        if writes:
            errors.append(f"{cmd}: @hints read-only, but the code writes: "
                          + "; ".join(f"{what} ({where})" for what, where in writes[:3]))
        read_only = not writes
    # `additive` or `destructive` says the command writes, whatever its verb: get_level_blueprint
    # creates the Level Blueprint when asked to.
    if "additive" in tokens or "destructive" in tokens:
        if "read-only" in tokens:
            errors.append(f"{cmd}: @hints read-only together with additive or destructive")
        read_only = False
    if read_only:
        annotations = {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True,
                       "openWorldHint": network}
    else:
        additive = (verb in ADDITIVE_VERBS and not any(e[0] == "deletes" for e in writes)
                    and not replaces_something(description) and not (param_names & REPLACE_PARAMS))
        if "additive" in tokens:
            additive = True
        if "destructive" in tokens:
            additive = False
        idempotent = verb in IDEMPOTENT_VERBS or bool(IDEMPOTENT_RE.search(description))
        if "idempotent" in tokens:
            idempotent = True
        if "not-idempotent" in tokens or cmd in ARBITRARY:
            idempotent = False
        annotations = {"readOnlyHint": False, "destructiveHint": not additive,
                       "idempotentHint": idempotent, "openWorldHint": network or cmd in ARBITRARY}
    if "open-world" in tokens:
        annotations["openWorldHint"] = True
    if "closed-world" in tokens:
        annotations["openWorldHint"] = False
    return annotations, errors


def classify_catalog(commands: dict[str, dict]) -> tuple[dict[str, dict], list[str]]:
    """{cmd: annotations} for the catalog entries {cmd: {description, params}}, and the errors."""
    evidence = scan_evidence()
    tags = scan_hint_tags()
    out, errors = {}, []
    for cmd, entry in commands.items():
        names = {p["name"] for p in entry.get("params", [])}
        ann, errs = classify(cmd, entry.get("description", ""), names, evidence.get(cmd, []), tags.get(cmd, []))
        out[cmd] = ann
        errors += errs
    for cmd in sorted(set(tags) - set(commands)):
        errors.append(f"{cmd}: @hints on a command no route table registers")
    return out, errors


def run_controls(verdicts: dict[str, dict]) -> bool:
    checks = ([(c, "readOnlyHint", True) for c in POSITIVE_READ_ONLY]
              + [(c, "readOnlyHint", False) for c in NOT_READ_ONLY]
              + [(c, "destructiveHint", True) for c in DESTRUCTIVE]
              + [(c, "destructiveHint", False) for c in ADDITIVE])
    ok = True
    for cmd, key, want in checks:
        got = verdicts.get(cmd, {}).get(key)
        hit = got == want
        ok &= hit
        print("  %-32s %-16s %-5s %s" % (cmd, key, want, "OK" if hit else f"REFUTED: {got}"))
    return ok


def _catalog_commands() -> dict[str, dict]:
    import json
    with open(_paths.CATALOG_JSON, encoding="utf-8") as f:
        cat = json.load(f)
    return {c["name"]: c for d in cat["domains"] for c in d["commands"]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--list", action="store_true", help="every command with its hints")
    ap.add_argument("--controls", action="store_true", help="run the controls and stop")
    ap.add_argument("--why", metavar="CMD", help="one command's verdict and evidence")
    args = ap.parse_args()

    commands = _catalog_commands()
    verdicts, errors = classify_catalog(commands)
    if args.why:
        evidence = scan_evidence().get(args.why, [])
        print(args.why, "verb:", verb_of(args.why), verdicts.get(args.why))
        for what, where in evidence:
            print(f"  {what:38} {where}")
        return 0
    print("CONTROLS:")
    ok = run_controls(verdicts)
    if args.controls:
        return 0 if ok else 1
    counts = Counter()
    for v in verdicts.values():
        if v["readOnlyHint"]:
            counts["read-only"] += 1
        else:
            counts["destructive" if v["destructiveHint"] else "additive"] += 1
            counts["idempotent writes"] += v["idempotentHint"]
        counts["open world"] += v["openWorldHint"]
    print("\n%d commands: %s" % (len(verdicts), ", ".join(f"{k} {n}" for k, n in counts.items())))
    if args.list:
        for cmd in sorted(verdicts):
            v = verdicts[cmd]
            flags = [k.replace("Hint", "") for k, val in v.items() if val]
            print(f"  {cmd:48} {' '.join(flags)}")
    for e in errors:
        print("ERROR:", e)
    return 1 if errors or not ok else 0


if __name__ == "__main__":
    sys.exit(main())
