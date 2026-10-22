"""codegen_pie_safety.py — emits the PIE-UNSAFE command table for the plugin.

Why this exists
---------------
With a Play-in-Editor session live, two families of MCP commands stop being
trustworthy, each for its own measured reason:

  1. ANYTHING THAT COMPILES A BLUEPRINT -> the editor can DIE.
     Compiling reinstances every live actor of that class, and the reinstancer
     leaves a `UREINST_<Class>` trampoline behind. If one of those retains a
     world, `UEditorEngine::CheckForWorldGCLeaks` logs Fatal (not Error:
     Editor.CheckForWorldGCLeaksAreFatal defaults to true) and the process is
     gone, taking every in-memory edit with it. Observed as a "World Memory
     Leaks" crash seconds after a co-op PIE start, with a
     `UREINST_<WidgetBlueprint>_C` trampoline named as the reference holding
     the world.

  2. ANYTHING THAT SAVES -> a silent FALSE POSITIVE.
     The engine guard `Editor.AllowSavingAssetsDuringPIE=0` refuses the write
     BEFORE attempting anything, and the handler's `{saved:false}` payload used
     to come back wrapped in the bridge's synthesized `status:success`. Worse,
     `list_dirty_packages.pie_active` has been seen to return false with PIE
     live and answering, so a caller that uses that field as its "safe to
     save" check walks straight into the error.

A refused command is a visible, recoverable error. A dead editor is a lost
session, and a save that reports success without writing is drift nobody sees
until it matters. Hence: refuse, and say so.

Where the list comes from -- and why it is NOT hand-written
-----------------------------------------------------------
Family (1) is a property OF THE C++, so it is DERIVED from the C++ (the same
attribution method as Tools/audit_runtime_mode.py, and for the same reason:
a hand-kept list silently rots the moment someone adds a command).

  cmd -> handler   `{ TEXT("x"), &HandleX }` in the family's route table
  handler -> body  from `TSharedPtr<FJsonObject> HandleX(` to the first
                   line starting with `}` at column 0 (Epic's closing style)
  body -> verdict  the body, OR any same-file helper it calls (one level), names
                   a compile trigger

Every emitted row carries its evidence (file + function + marker), so the table is
auditable instead of believed -- and it does not change when an unrelated edit moves lines. Commands whose handler could not be located are
REPORTED, never classified blind.

Family (2) is a handful of saves that compile nothing, so no scan can find them;
they are the CURATED block below, each with its one line of evidence.

Controls (run with --controls; they gate --write)
------------------------------------------------
POSITIVE: compile_blueprint, add_graph_node, connect_pins, reparent_blueprint,
build_blueprint_graph, remove_graph_node must be classified unsafe.
NEGATIVE: get_graph_nodes, get_blueprint_info, get_blueprint_functions, get_editor_status must NOT.
A scan that cannot reproduce those is refuted, and refuses to write.

Note the two commands that READ like readers and genuinely compile:
`audit_blueprint_health` and `find_erroring_blueprints`. They
are true positives -- verify against the cited function before "fixing" them out.

What it emits
-------------
  * Source/UEAgent/Public/McpPieSafety.gen.h
  * Source/UEAgent/Private/McpPieSafety.gen.cpp

The same table decides which tools get a `force` parameter in the schema the
plugin's MCP server publishes (McpToolCatalog): without it the guard's override
valve would be unreachable from the tool -- the worst kind of guard, one you
cannot get out of.

ASCII-only (MSVC C4819 on non-ASCII source bytes) and sorted, so an unchanged
tree reproduces both files byte for byte -- which is what --check relies on.

Usage:
  python Tools/codegen_pie_safety.py             # report only (default)
  python Tools/codegen_pie_safety.py --controls  # run the controls and stop
  python Tools/codegen_pie_safety.py --write     # regenerate the .h/.cpp
  python Tools/codegen_pie_safety.py --check     # exit non-zero if the files drifted
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import _paths  # noqa: E402
from check_copyright import NOTICE  # noqa: E402

ROOT = _paths.PLUGIN_ROOT_P
PLUGIN = _paths.SOURCE_DIR_P
CMDS_DIR = PLUGIN / "Private" / "Commands"
OUT_H = PLUGIN / "Public" / "McpPieSafety.gen.h"
OUT_CPP = PLUGIN / "Private" / "McpPieSafety.gen.cpp"

# --------------------------------------------------------------------------
# Compile triggers. The first three are the ones that reinstance; CreateBlueprint
# is included because a Blueprint born during PIE cannot be saved either (that is
# create_blueprint's own measured behaviour, UEAgentBlueprintCommands.cpp).
# --------------------------------------------------------------------------
MARKERS = {
    "FKismetEditorUtilities::CompileBlueprint": "compiles",
    "FBlueprintCompilationManager::CompileSynchronously": "compiles",
    "FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified": "reinstances",
    "FBlueprintEditorUtils::MarkBlueprintAsModified": "reinstances",
    "FKismetEditorUtilities::CreateBlueprint": "creates",
}

# --------------------------------------------------------------------------
# CURATED: saves. They compile nothing, so the scan cannot see them; the engine
# guard is what makes them lie. One line of evidence each -- no entry without one.
# --------------------------------------------------------------------------
CURATED = {
    "save_asset": "Editor.AllowSavingAssetsDuringPIE=0 refuses before trying; saved:false came back as status:success",
    "save_loaded_asset": "same engine guard as save_asset",
    "save_level": "same engine guard; save_level also duplicates the world and can leave it orphaned in memory",
    "save_all": "same engine guard, over every dirty package at once",
}

# Never gated, whatever the scan says: refusing these would make the refusal
# itself unrecoverable (you could not turn PIE off to comply).
NEVER_GATE = {
    "stop_play_in_editor",
    "start_play_in_editor",
    "is_play_in_editor_active",
    "wait_for_pie_completion",
}

POSITIVE_CONTROLS = [
    "compile_blueprint", "add_graph_node", "connect_pins",
    "reparent_blueprint", "build_blueprint_graph", "remove_graph_node",
]
NEGATIVE_CONTROLS = [
    "get_graph_nodes", "get_blueprint_info", "get_blueprint_functions", "get_editor_status",
]

# A family's route table: `{ TEXT("cmd"), &HandleXxx },`
ROUTE_RE = re.compile(r'\{\s*TEXT\("([a-z0-9_]+)"\)\s*,\s*&\s*(?:\w+::)?(Handle[A-Za-z0-9_]+)\s*\}')
# Definitions at column 0, free (`TSharedPtr<...> HandleX(`) or qualified
# (`FClass::Method(`). A handler whose definition is missed has no body -- and a
# command without a body is not classified, which reads as "safe".
FUNC_DEF_RE = re.compile(
    r'^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?\b(?:[A-Za-z_][A-Za-z0-9_]*::)?'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\(')
# Definitions INSIDE a namespace are indented: a return type, then the name. Their
# body ends at the `}` with the definition's own indentation. Without them, a
# handler whose compile or save sits in a namespace helper reads as "safe".
INDENTED_DEF_RE = re.compile(
    r'^([ \t]+)(?!(?:return|else|if|for|while|switch|case|new|delete|throw)\b)'
    r'(?:[A-Za-z_][A-Za-z0-9_:<>,\*&]*\s+)+(?:[A-Za-z_][A-Za-z0-9_]*::)?([A-Za-z_][A-Za-z0-9_]*)\s*\(')
LOCAL_CALL_RE = re.compile(r'\b([A-Z][A-Za-z0-9_]*)\s*\(')
# Calls into the shared helpers, followed into Common/McpCommonUtils.cpp.
SHARED_CALL_RE = re.compile(r'\bmcp_common::([A-Z][A-Za-z0-9_]*)\s*\(')
SHARED_REL = "Common/McpCommonUtils.cpp"

# Reason codes mirrored in the .h enum. Order = severity.
REASON_ORDER = ["compiles", "reinstances", "creates", "saves"]


def function_bodies(lines):
    """{func_name: (first_line_no, body_lines)} for definitions at column 0 and inside namespaces.

    Skips DECLARATIONS: a signature that reaches `;` before `{` has no body, and
    keeping it (files put declarations at the top, before the definition) would
    give an empty body -- i.e. a command "without triggers" only because nobody
    looked where the triggers are.
    """
    out = {}
    i, n = 0, len(lines)
    while i < n:
        m = FUNC_DEF_RE.match(lines[i])
        indent, name = ("", m.group(1)) if m else ("", None)
        if not m:
            mi = INDENTED_DEF_RE.match(lines[i])
            if mi:
                indent, name = mi.group(1), mi.group(2)
        if not name:
            i += 1
            continue

        # signature -> body or semicolon, looking a few lines ahead
        k, is_definition = i, False
        while k < min(i + 8, n):
            if "{" in lines[k]:
                is_definition = True
                break
            if ";" in lines[k]:
                break
            k += 1
        if not is_definition:
            i += 1
            continue

        j = k + 1
        while j < n and not lines[j].startswith(indent + "}"):
            j += 1
        out.setdefault(name, (i + 1, lines[i:j + 1]))
        i = j + 1
    return out


def markers_in(body_lines, start_line, func):
    hits = []
    for off, ln in enumerate(body_lines):
        if ln.lstrip().startswith("//"):
            continue
        for marker, reason in MARKERS.items():
            if marker in ln:
                hits.append((reason, start_line + off, func, marker))
    return hits


def scan():
    """-> (verdicts, unmapped). verdicts: cmd -> (reason, 'file function: marker')."""
    raw = defaultdict(list)
    unmapped = []
    shared = function_bodies((CMDS_DIR / SHARED_REL).read_text(encoding="utf-8", errors="replace").splitlines())

    for cpp in sorted(CMDS_DIR.rglob("*.cpp")):
        lines = cpp.read_text(encoding="utf-8", errors="replace").splitlines()
        bodies = function_bodies(lines)
        rel = cpp.relative_to(CMDS_DIR).as_posix()

        for ln in lines:
            rm = ROUTE_RE.search(ln)
            if not rm:
                continue
            cmd, handler = rm.group(1), rm.group(2)
            if handler not in bodies:
                unmapped.append((cmd, handler, rel))
                continue

            start, body = bodies[handler]
            for reason, line, func, marker in markers_in(body, start, handler):
                raw[cmd].append((reason, rel, line, func, marker))

            # one level of transitivity: same-file helpers and sub-commands
            texts = ["\n".join(body)]
            for callee in set(LOCAL_CALL_RE.findall("\n".join(body))):
                if callee == handler or callee not in bodies:
                    continue
                cstart, cbody = bodies[callee]
                texts.append("\n".join(cbody))
                for reason, line, func, marker in markers_in(cbody, cstart, callee):
                    raw[cmd].append((reason, rel, line, func, marker))

            # the mcp_common helpers the handler, or one of those helpers, calls
            for callee in {name for text in texts for name in SHARED_CALL_RE.findall(text)}:
                if callee in shared:
                    cstart, cbody = shared[callee]
                    for reason, line, func, marker in markers_in(cbody, cstart, callee):
                        raw[cmd].append((reason, SHARED_REL, line, func, marker))

    verdicts = {}
    for cmd, hits in raw.items():
        if cmd in NEVER_GATE:
            continue
        # the most severe reason; at equal severity the command's own file is the evidence
        hits.sort(key=lambda h: (REASON_ORDER.index(h[0]), h[1] == SHARED_REL, h[1], h[2]))
        reason, rel, _line, func, marker = hits[0]
        verdicts[cmd] = (reason, "%s %s: %s" % (rel, func, marker))

    for cmd, evidence in CURATED.items():
        if cmd not in NEVER_GATE:
            verdicts[cmd] = ("saves", evidence)

    return verdicts, unmapped


def run_controls(verdicts):
    ok = True
    print("POSITIVE CONTROL (must come out unsafe):")
    for c in POSITIVE_CONTROLS:
        hit = c in verdicts
        ok &= hit
        print("  %-28s %s" % (c, "OK" if hit else "REFUTED: not classified"))
    print("NEGATIVE CONTROL (must come out safe):")
    for c in NEGATIVE_CONTROLS:
        hit = c not in verdicts
        ok &= hit
        print("  %-28s %s" % (c, "OK" if hit else "REFUTED: classified unsafe"))
    return ok


def c_escape(s: str) -> str:
    ascii_only = s.encode("ascii", "replace").decode("ascii")
    return ascii_only.replace("\\", "\\\\").replace('"', '\\"')


HEADER_BANNER = NOTICE + "\n\n" + """// AUTO-GENERATED by Tools/codegen_pie_safety.py from the C++ in Commands/ -- DO NOT EDIT BY HAND.
// To regenerate:  python Tools/codegen_pie_safety.py --write
//
"""


def emit_h(verdicts):
    return HEADER_BANNER + """// Public API of the PIE-unsafe command table (declarations only; data in the .cpp).
//
// The problem it solves: with PIE live, compiling a Blueprint can KILL the editor
// (the reinstancer's UREINST_* retains a world and CheckForWorldGCLeaks is Fatal)
// and saving returns SUCCESS WITHOUT WRITING (Editor.AllowSavingAssetsDuringPIE=0).
// Both fail silently.
//
// The dispatcher checks this table ONCE per command and REFUSES with an envelope
// that says to stop PIE. `force: true` in the params bypasses it -- the same valve
// the bridge's PIE ownership guard already uses.
//
// Coverage: %d commands (%d derived from the C++, %d curated).
#pragma once

#include "CoreMinimal.h"

namespace McpPieSafety
{
    /** Why the command cannot be trusted while PIE is active. */
    enum class EReason : uint8
    {
        /** Compiles a Blueprint -> reinstancing -> can kill the editor. */
        Compiles,
        /** Marks the Blueprint modified -> the same reinstancing, deferred. */
        Reinstances,
        /** Creates a Blueprint that cannot be persisted while PIE is active. */
        Creates,
        /** Saves: the engine guard refuses before trying, silently. */
        Saves,
    };

    struct FEntry
    {
        EReason Reason;
        /** File and function of the trigger, or the one-line evidence of a curated entry. */
        const TCHAR* Evidence;
    };

    /**
     * The entry of CommandName, or nullptr when the command is NOT in the table.
     * nullptr means "not known to be unsafe", never "verified safe".
     * The key is an FName, so the lookup is case-insensitive, like the one of
     * FJsonObject.
     */
    UEAGENT_API const FEntry* Find(FName CommandName);

    /** Short text for the refusal envelope ("compiles a Blueprint", ...). */
    UEAGENT_API const TCHAR* ReasonText(EReason Reason);
}
""" % (len(verdicts), len(verdicts) - len(CURATED), len(CURATED))


def emit_cpp(verdicts):
    rows = []
    for cmd in sorted(verdicts):
        reason, evidence = verdicts[cmd]
        enum = {"compiles": "Compiles", "reinstances": "Reinstances",
                "creates": "Creates", "saves": "Saves"}[reason]
        rows.append('    { TEXT("%s"), EReason::%s, TEXT("%s") },'
                    % (cmd, enum, c_escape(evidence)))
    body = "\n".join(rows)

    return HEADER_BANNER + """// Data + lookups of the PIE-unsafe table.
#include "McpPieSafety.gen.h"

namespace McpPieSafety
{
// Named (NOT anonymous) namespace on purpose: this module builds with bUseUnity =
// true and anonymous-namespace helpers have collided across unity blobs in this
// plugin before (see bUseUnity in UEAgent.Build.cs).
namespace Generated
{

struct FRow
{
    const TCHAR* Command;
    EReason      Reason;
    const TCHAR* Evidence;
};

// Sorted by command name. Each row carries the code that PROVES it -- a row whose
// evidence no longer resolves is a row to delete, not to trust.
static const FRow GRows[] =
{
%s
};

static const TMap<FName, FEntry>& GetTable()
{
    // Built ONCE on first use, never per call.
    static const TMap<FName, FEntry> Table = []()
    {
        TMap<FName, FEntry> Out;
        Out.Reserve(UE_ARRAY_COUNT(GRows));
        for (const FRow& Row : GRows)
        {
            Out.Add(FName(Row.Command), FEntry{ Row.Reason, Row.Evidence });
        }
        return Out;
    }();
    return Table;
}

} // namespace Generated

const FEntry* Find(FName CommandName)
{
    return Generated::GetTable().Find(CommandName);
}

const TCHAR* ReasonText(EReason Reason)
{
    switch (Reason)
    {
    case EReason::Compiles:    return TEXT("compiles a Blueprint");
    case EReason::Reinstances: return TEXT("marks a Blueprint modified, which reinstances it");
    case EReason::Creates:     return TEXT("creates a Blueprint that cannot be persisted during PIE");
    case EReason::Saves:       return TEXT("writes an asset to disk");
    default:                   return TEXT("is not safe during PIE");
    }
}

} // namespace McpPieSafety
""" % body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="regenerate the .h/.cpp")
    ap.add_argument("--check", action="store_true", help="exit 1 if the files drifted")
    ap.add_argument("--controls", action="store_true", help="run the controls and stop")
    ap.add_argument("--list", action="store_true", help="print every classified command")
    args = ap.parse_args()

    verdicts, unmapped = scan()

    by_reason = defaultdict(int)
    for reason, _ in verdicts.values():
        by_reason[reason] += 1

    print("PIE-UNSAFE: %d commands" % len(verdicts))
    for reason in REASON_ORDER:
        if by_reason[reason]:
            print("  %-14s %d" % (reason, by_reason[reason]))
    print("handlers not located (review by hand): %d" % len(unmapped))
    for cmd, handler, rel in unmapped:
        print("    UNMAPPED %-40s %-40s %s" % (cmd, handler, rel))
    print()

    controls_ok = run_controls(verdicts)
    if args.controls:
        return 0 if controls_ok else 1

    if args.list:
        print()
        for cmd in sorted(verdicts):
            reason, evidence = verdicts[cmd]
            print("  %-44s %-12s %s" % (cmd, reason, evidence))

    outputs = ((OUT_H, emit_h(verdicts)),
               (OUT_CPP, emit_cpp(verdicts)))

    if args.check:
        drift = [p.name for p, new in outputs
                 if (p.read_text(encoding="ascii") if p.exists() else None) != new]
        if drift:
            print("\nDRIFT: %s -- run --write" % ", ".join(drift))
            return 1
        print("\nno drift")
        return 0

    if args.write:
        if not controls_ok:
            print("\nREFUSED: the controls fail; the scanner is refuted, "
                  "nothing is written.")
            return 1
        for path, new in outputs:
            # newline="\n", as the other generators: on Windows the default would write CRLF.
            path.write_text(new, encoding="ascii", newline="\n")
            print("\n-> %s" % path, end="")
        print()
        return 0

    print("\n(report only; --write to regenerate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
