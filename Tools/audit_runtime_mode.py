"""Audit the RUNTIME MODE of every MCP command (headless-safe / capability-gated / live-only).

Derives the classification from the C++ (the source of truth): for each catalog command
it locates the real handler and scans its body for the *gates* the plugin uses to tell
runtime capabilities apart. It is NOT an oracle: it is a heuristic detector that **cites
the evidence** (pattern + line) so it can be audited. The "honest headless" rule (never
fake a success when running headless) lives in the code, not in a hand-kept table; this
makes it visible.

Three classes (priority live > capability > headless):

  live_only         uses interactive selection / PIE / the editor viewport, or an honest
                    comment such as "cannot be invoked headless". Headless, it returns a
                    clean error or a payload with a `note` (it does not pretend).
  capability_gated  branches on a runtime capability: FApp::CanEverRender() /
                    GIsRHIInitialized (GPU), IsRunningCommandlet, EPythonCommandFlags::Unattended.
                    Does the real effect when it can; reports honestly when it cannot.
  headless_safe     no gates detected -> assumed to run the same headless or live.

Attribution method (reliable, independent of the annotations):
  1. cmd -> function:  the family's route table `{ TEXT("x"), &HandleX }`.
  2. function -> body: from the signature `TSharedPtr<FJsonObject> HandleX(...)` to
     the first line starting with `}` at column 0 (Epic style: end of function).

Known limits (reported, not hidden):
  * `GetSelectedActors` and friends are sometimes a convenience *fallback* over an
    explicit param -> they can over-flag live_only. That is why the match is cited: review it.
  * A cmd whose handler could not be located (non-standard dispatch, inline body) falls
    into UNMAPPED for manual review; it is never classified blind.

Usage:
    python Tools/audit_runtime_mode.py                 # Markdown report to stdout
    python Tools/audit_runtime_mode.py --json out.json # + structured JSON
    python Tools/audit_runtime_mode.py --md out.md     # + Markdown to a file
    python Tools/audit_runtime_mode.py --domain Audio   # filter one domain
    python Tools/audit_runtime_mode.py --list-headless  # also list the headless_safe commands
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import _paths  # noqa: E402

ROOT = _paths.PLUGIN_ROOT_P
PROJECT = ROOT
CMDS_DIR = _paths.COMMANDS_DIR_P
CATALOG = _paths.CATALOG_JSON_P
# Optional folder of Python coverage tests whose `allow_errors=[...]` declarations are
# used as corroboration. When it is absent (the default), that report section is empty.
COV_DIR = ROOT / "Intermediate" / "Codegen" / "cov"

# --- C++ parsing ------------------------------------------------------------ #
SIG_RE = re.compile(r"^\s*TSharedPtr<FJsonObject>\s+(?:\w+::)?(Handle\w+)\s*\([^;]*$")
# cmd -> Handle: the family's route table, `{ TEXT("cmd"), &HandleXxx },`
ROUTE_RE = re.compile(r'\{\s*TEXT\("([^"]+)"\)\s*,\s*&\s*(?:\w+::)?(Handle\w+)\s*\}')

# --- gates: (label, regex). The order of the two lists sets the priority. ---- #
LIVE_PATTERNS = [
    ("selection",
     re.compile(r"GetSelected(?:Actors|Objects|Components|Assets|Folders|Set)\b|\bUSelection\b")),
    ("pie",
     re.compile(r"\bStartPlayInEditorSession\b|\bRequestPlaySession\b|\bPlayInEditor\b|\bIsPlayInEditor\w*")),
    ("viewport-interactive",
     re.compile(r"GCurrentLevelEditingViewportClient|FLevelEditorViewportClient|GetActiveLevelViewport")),
    # Only prose that declares GENUINE interactivity. Does NOT include "requires the
    # editor": that phrase is the `#else` of an `#if WITH_EDITOR` guard (missing in game
    # builds), but WITH_EDITOR is 1 in the headless editor too -> those cmds run headless.
    ("comment-interactive",
     re.compile(r"cannot be invoked headless|without a selection|needs? a selection|"
                r"interactive (?:menu|selection|right-click|context)|"
                r"from the (?:right-click|context) menu", re.I)),
]
GATE_PATTERNS = [
    ("render-gate", re.compile(r"\bCanEverRender\b|\bGIsRHIInitialized\b")),
    ("commandlet-gate", re.compile(r"\bIsRunningCommandlet\b")),
    ("automation-gate", re.compile(r"\bGIsAutomationTesting\b")),
    ("unattended-flag", re.compile(r"EPythonCommandFlags::Unattended|\bUnattended\b")),
    # Honest degradation declared in prose: the handler detects that it CANNOT work
    # headless (render / nullrhi / authoring) and says so instead of faking success.
    ("comment-degraded",
     re.compile(r"cannot render \([^)]*headless|nullrhi|not authorable headless|"
                r"cannot be authored headless|cannot be set synchronously headless|"
                r"headless / RHI not initialized|reporting (?:the )?existing cached", re.I)),
]
HEADLESS_POS = re.compile(r"headless[- ]safe|fully headless", re.I)
# Compile guard `#else ... requires the editor (WITH_EDITOR)`: NOT live-only. The cmd
# runs in ANY editor (headless included); it is only missing from game builds.
WEDITOR_GUARD = re.compile(r"requires? the editor \(WITH_EDITOR", re.I)

# --- corroboration from the coverage tests ---------------------------------- #
ALLOW_RE = re.compile(r"allow_errors\s*=\s*\[(.*?)\]", re.S)
EDITOR_PHRASE = re.compile(
    r"requires? the editor|needs? the editor|in the editor|headless|\bPIE\b|"
    r"play.?in.?editor|interactive|selection|cannot\b[^\n]*\bheadless", re.I)


def parse_file(path: pathlib.Path):
    """Returns (bodies: {fn: (body_text, start_line)}, dispatch: [(cmd, fn)])."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    bodies: dict[str, tuple[str, int]] = {}
    sigs = [(m.group(1), i) for i, ln in enumerate(lines) if (m := SIG_RE.match(ln))]
    for fn, i in sigs:
        j = i + 1
        while j < len(lines) and not lines[j].startswith("}"):
            j += 1
        # first definition wins (Handle* functions are not overloaded); duplicates ignored.
        bodies.setdefault(fn, ("\n".join(lines[i:j + 1]), i + 1))
    text = "\n".join(lines)
    dispatch = ROUTE_RE.findall(text)
    return bodies, dispatch


def collect(patterns, body: str, start_line: int):
    hits = []
    for label, rx in patterns:
        m = rx.search(body)
        if m:
            line = start_line + body[:m.start()].count("\n")
            hits.append({"label": label, "line": line, "match": m.group(0)[:48]})
    return hits


def classify(body: str, start_line: int):
    live = collect(LIVE_PATTERNS, body, start_line)
    gate = collect(GATE_PATTERNS, body, start_line)
    if live:
        klass = "live_only"
    elif gate:
        klass = "capability_gated"
    else:
        klass = "headless_safe"
    flags = []
    if WEDITOR_GUARD.search(body):
        flags.append("with_editor_guard")   # editor-only build; still runs headless
    if HEADLESS_POS.search(body):
        flags.append("headless-safe-comment")
    return klass, live + gate, flags


def load_catalog():
    data = json.loads(CATALOG.read_text(encoding="utf-8"))
    cmd2domain = {}
    for dom in data.get("domains", []):
        for cmd in dom.get("commands", []):
            cmd2domain[cmd["name"]] = dom["domain"]
    return cmd2domain


def scan_cpp(cmd2domain):
    """Classify every cmd. Returns (records, unmapped, extra_in_cpp, dup)."""
    records = {}          # cmd -> record
    seen_in_cpp = set()
    dup = []
    for path in sorted(CMDS_DIR.rglob("*.cpp")):
        bodies, dispatch = parse_file(path)
        rel = path.relative_to(PROJECT).as_posix()
        for cmd, fn in dispatch:
            if cmd in seen_in_cpp:
                dup.append({"cmd": cmd, "file": rel, "fn": fn})
                continue
            seen_in_cpp.add(cmd)
            entry = bodies.get(fn)
            if entry is None:
                # dispatch to a function whose body was not located: manual review.
                records[cmd] = {
                    "cmd": cmd, "domain": cmd2domain.get(cmd, "?"), "klass": "unmapped",
                    "file": rel, "fn": fn, "evidence": [], "flags": [],
                    "reason": "dispatch without a locatable function body"}
                continue
            body, start = entry
            klass, ev, flags = classify(body, start)
            records[cmd] = {
                "cmd": cmd, "domain": cmd2domain.get(cmd, "?"), "klass": klass,
                "file": rel, "fn": fn, "evidence": ev, "flags": flags}
    unmapped = [{"cmd": c, "domain": d, "reason": "no dispatch in the scanned C++"}
                for c, d in sorted(cmd2domain.items()) if c not in records]
    extra = sorted(c for c in records if c not in cmd2domain)
    return records, unmapped, extra, dup


def scan_cov():
    """Sites in the coverage tests where an allow_errors declares an editor/PIE/headless gate."""
    hits = []
    if not COV_DIR.exists():
        return hits
    for path in sorted(COV_DIR.rglob("*.py")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in ALLOW_RE.finditer(text):
            inner = m.group(1)
            if EDITOR_PHRASE.search(inner):
                line = text[:m.start()].count("\n") + 1
                snippet = " ".join(inner.split())[:90]
                hits.append({"file": path.relative_to(PROJECT).as_posix(),
                             "line": line, "allow_errors": snippet})
    return hits


def ev_str(evidence):
    return ", ".join(f"`{e['label']}`@{e['line']}" for e in evidence) or "—"


def build_markdown(records, unmapped, extra, dup, cov_hits, domain_filter, list_headless):
    recs = list(records.values())
    if domain_filter:
        recs = [r for r in recs if r["domain"].lower() == domain_filter.lower()]
    by_class = defaultdict(list)
    for r in recs:
        by_class[r["klass"]].append(r)
    counts = Counter(r["klass"] for r in recs)

    out = []
    out.append("# Runtime mode of the MCP commands (headless-safe / capability-gated / live-only)\n")
    out.append("> **AUTO-GENERATED** — do not edit by hand. Regenerate with "
               "`python Tools/audit_runtime_mode.py --md <file>`. "
               "Check the invariants when regenerating: 0 UNMAPPED and no new dispatch "
               "collisions.\n")
    out.append("> Derived from the C++ by `Tools/audit_runtime_mode.py`. Heuristic with cited "
               "evidence (pattern@line); NOT an absolute verdict — review the evidence.\n")
    if domain_filter:
        out.append(f"**Domain filter:** `{domain_filter}`\n")

    total = sum(counts.values())
    out.append("## Summary\n")
    out.append("| Class | Commands |")
    out.append("|---|---:|")
    for k in ("live_only", "capability_gated", "headless_safe", "unmapped"):
        out.append(f"| {k} | {counts.get(k, 0)} |")
    out.append(f"| **total** | **{total}** |\n")

    def table(klass, title, note=""):
        rows = sorted(by_class.get(klass, []), key=lambda r: (r["domain"], r["cmd"]))
        out.append(f"## {title} — {len(rows)}\n")
        if note:
            out.append(note + "\n")
        if not rows:
            out.append("_(none)_\n")
            return
        out.append("| Command | Domain | Evidence (pattern@line) | Handler |")
        out.append("|---|---|---|---|")
        for r in rows:
            out.append(f"| `{r['cmd']}` | {r['domain']} | {ev_str(r['evidence'])} | "
                       f"{r['file'].split('/')[-1]}::{r['fn']} |")
        out.append("")

    table("live_only", "live_only (needs the interactive editor / PIE / a selection)",
          "Headless (empty selection / no viewport), they must return a clean error or a "
          "payload with a `note`. Check it in the coverage tests with `allow_errors=[\"no selection\", "
          "\"requires a viewport\", ...]`.")
    table("capability_gated", "capability_gated (honest degradation by runtime capability)",
          "They do the real effect when the capability exists (GPU/RHI) and report honestly when it does not.")

    hs = by_class.get("headless_safe", [])
    n_guard = sum(1 for r in hs if "with_editor_guard" in r["flags"])
    out.append(f"## headless_safe — {counts.get('headless_safe', 0)}\n")
    out.append("No runtime gates; assumed to behave the same headless or live. "
               f"Of these, **{n_guard}** carry a `WITH_EDITOR` guard (absent from Shipping/Game "
               "builds, but they run headless in the editor). Count per domain:\n")
    dom_counts = Counter(r["domain"] for r in hs)
    out.append("| Domain | Commands |")
    out.append("|---|---:|")
    for dom, n in sorted(dom_counts.items()):
        out.append(f"| {dom} | {n} |")
    out.append("")
    if list_headless:
        out.append("<details><summary>Full headless_safe list</summary>\n")
        for r in sorted(hs, key=lambda r: (r["domain"], r["cmd"])):
            star = f" · _{', '.join(r['flags'])}_" if r["flags"] else ""
            out.append(f"- `{r['cmd']}` ({r['domain']}){star}")
        out.append("\n</details>\n")

    if unmapped and not domain_filter:
        out.append(f"## UNMAPPED — {len(unmapped)} (manual review)\n")
        out.append("Catalog commands whose handler was not located by the standard pattern "
                   "(unconventional dispatch or body). Not classified blind.\n")
        for u in unmapped:
            out.append(f"- `{u['cmd']}` ({u['domain']}) — {u['reason']}")
        out.append("")

    if extra and not domain_filter:
        out.append(f"## Drift: C++ dispatch missing from the catalog — {len(extra)}\n")
        out.append("Run `python Tools/codegen_command_catalog.py --write` (the catalog must be "
                   "derived from the C++).\n")
        for c in extra:
            out.append(f"- `{c}`")
        out.append("")

    if dup and not domain_filter:
        out.append(f"## Dispatch collision — {len(dup)} (review: dead handler or delegation)\n")
        out.append("The same command dispatched with `CommandType==TEXT(...)` in more than one "
                   "handler class. The bridge routes to ONE; the other may be **(a)** dead code, "
                   "or **(b)** reached through cross-handler delegation from the live one (e.g. "
                   "`spawn_blueprint_actor`: EditorCommands delegates to BlueprintCommands). "
                   "Confirm it against the bridge. The class above was taken from the first file "
                   "(alphabetical order), which may be the non-canonical handler.\n")
        for d in dup:
            out.append(f"- `{d['cmd']}` also in {d['file']}::{d['fn']}")
        out.append("")

    out.append(f"## Corroboration in the coverage tests — {len(cov_hits)} editor/PIE/headless gates\n")
    out.append("Sites where an `allow_errors` declares an honest gate. Hints to cross-check with "
               "the live_only/capability_gated commands above (not mapped 1:1 to the cmd).\n")
    if cov_hits:
        out.append("| Test | Line | allow_errors |")
        out.append("|---|---:|---|")
        for h in cov_hits:
            out.append(f"| {h['file'].split('tests/cov/')[-1]} | {h['line']} | {h['allow_errors']} |")
        out.append("")
    else:
        out.append("_(none)_\n")

    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Audit the runtime mode of the MCP commands.")
    ap.add_argument("--json", metavar="PATH", help="write the structured report as JSON")
    ap.add_argument("--md", metavar="PATH", help="write the Markdown report to a file")
    ap.add_argument("--domain", metavar="NAME", help="filter to a single domain")
    ap.add_argument("--list-headless", action="store_true",
                    help="also list the headless_safe commands in the Markdown")
    args = ap.parse_args(argv)

    if not CATALOG.exists():
        sys.exit(f"ERROR: catalog not found at {CATALOG}")
    if not CMDS_DIR.exists():
        sys.exit(f"ERROR: Commands tree not found at {CMDS_DIR}")

    cmd2domain = load_catalog()
    records, unmapped, extra, dup = scan_cpp(cmd2domain)
    cov_hits = scan_cov()

    md = build_markdown(records, unmapped, extra, dup, cov_hits, args.domain, args.list_headless)

    if args.json:
        payload = {
            "summary": dict(Counter(r["klass"] for r in records.values())),
            "total_catalog": len(cmd2domain),
            "commands": sorted(records.values(), key=lambda r: (r["klass"], r["domain"], r["cmd"])),
            "unmapped": unmapped,
            "extra_in_cpp": extra,
            "name_collisions": dup,
            "cov_editor_gates": cov_hits,
        }
        pathlib.Path(args.json).write_text(json.dumps(payload, indent=2, ensure_ascii=False),
                                           encoding="utf-8")
        print(f"JSON written to {args.json}", file=sys.stderr)
    if args.md:
        pathlib.Path(args.md).write_text(md, encoding="utf-8")
        print(f"Markdown written to {args.md}", file=sys.stderr)
    if not args.md:
        print(md)


if __name__ == "__main__":
    main()
