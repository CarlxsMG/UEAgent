"""Audit the OUTCOME HONESTY of every mutating MCP command.

One question: when a tool that **changes something** answers `success: true`, was
that `true` produced by a **real observation** (a re-read, a counter, an engine return
value), **honestly declared as unverifiable**, or is it **hardcoded**?

Three classes over the commands with a mutating verb (priority verified > declared >
hardcoded):

  verified              the handler derives the outcome from a measured boolean:
                        `mcp_common::MakeOutcomeReply(Data, bOk, Err)` / `MakeSavedReply(...)` /
                        `MakeVerifiedReply(...)` / `AddVerificationEvidence(...)`,
                        or publishes a computed `*verified` field.
  declared_unverifiable the handler ADMITS the limit in the response:
                        `EMcpVerification::NotVerifiable/NotAttempted`, a `note`/
                        `verification` field, a `*verified` set to `false`, or
                        explicit prose ("cannot verify", "best-effort"). No pretending.
  hardcoded_success     emits `MakeOkReply(...)` (or `success=true` by hand)
                        without either of the signals above. The `true` is a constant
                        in the code, not a measurement.

And two auxiliary classes that are NOT counted as debt, because counting them that
way would not be honest:
  delegated             the body only forwards to another command (`McpCommandFamilies::Run`); the
                        outcome comes from the target handler (audited on its own).
  no_outcome_signal     no success emission detected in the located body (tiny
                        handler, helper, or incomplete attribution).

Flags that are cited (they do not change the class, they explain it):
  echo:<params>         the response re-emits the SAME input parameters (the
                        `set_actor_team` pattern: `team_id = NewTeamId.GetId()`, i.e.
                        the input returned as if it were a read).
  literal-true:<fields> `SetBoolField(TEXT("applied"), true)` and family: the claim
                        that the effect happened, written by hand.
  mixed-success-paths   an honest path coexists with a bare `MakeOkReply`.
  outcome-literal-true  calls `MakeOutcomeReply(..., true, ...)`: uses the honest
                        helper but passes it a constant. Downgrades to hardcoded.

Attribution method: the SAME as `audit_runtime_mode.py` (`parse_file` +
`load_catalog`) -- cmd -> `HandleXxx` through the internal dispatch, and `HandleXxx` ->
body from the signature `TSharedPtr<FJsonObject> HandleXxx(` to the `}` at
column 0. Catalog commands it cannot attribute are listed in the report. Two kinds of
same-file helper count as part of the handler, one level deep: the one a signal-less
handler forwards to (`return Helper(...)`, flagged `via-helper:`), and the ones that
build the object it replies with (`Fill(Data, ...)`, `Data = Build(...)`, flagged
`via-reply-helper:`), whose honest and declared evidence is added to the handler's.

WHAT THIS DOES NOT COVER
------------------------
Read-only tools that return wrong data (a `get_*` that invents or caches) are invisible
to an *outcome* audit: there is no `success` to contrast. What it does cover is the
much larger population of **mutating** handlers whose `success:true` is a constant. It
is a heuristic detector with cited evidence (pattern@line), not an oracle: a handler
can compute `Result` from a real re-read and still end in `MakeOkReply` --
it will come out `hardcoded_success`, flagged as "review it", not as "it lies". Check
the evidence before believing the verdict.

Usage:
    python Tools/audit_outcome_honesty.py                  # Markdown report to stdout
    python Tools/audit_outcome_honesty.py --summary        # counters only
    python Tools/audit_outcome_honesty.py --json out.json  # + structured JSON
    python Tools/audit_outcome_honesty.py --md out.md      # + Markdown to a file
    python Tools/audit_outcome_honesty.py --domain Audio   # filter one domain
    python Tools/audit_outcome_honesty.py --cmd set_actor_team   # one command's record
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import audit_runtime_mode as arm  # noqa: E402  (same folder; imported after the sys.path insert)

ROOT = arm.ROOT              # plugin root
PROJECT = arm.PROJECT        # base of the relative paths in the report (= plugin root)
CMDS_DIR = arm.CMDS_DIR      # .../Private/Commands
PRIVATE_DIR = CMDS_DIR.parent

# --- which commands MUTATE? -------------------------------------------------- #
# Classified by the first token of the name (the verbs are the ones that occur in
# command_catalog.json). Ambiguous verbs are NOT forced into mutating: they are
# reported apart, because counting them would inflate the debt with commands that
# may not write anything.
MUTATING_VERBS = {
    "set", "add", "create", "remove", "delete", "clear", "rename", "configure",
    "connect", "disconnect", "duplicate", "apply", "spawn", "copy", "move",
    "build", "save", "compile", "draw", "generate", "import", "register",
    "bind", "batch", "update", "assign", "edit", "scaffold", "scale", "reparent",
    "reorder", "unregister", "inject", "commit", "link", "reimport", "bulk",
    "attach", "implement", "override", "place", "reset", "append", "rebuild",
    "recompute", "rotate", "translate", "flatten", "scatter", "destroy", "slice",
    "bake", "collapse", "setup", "sort", "wire", "ingest", "recombine",
    "reconcile", "split", "initialize", "align", "backup", "consolidate",
    "disable", "enable", "fixup", "migrate", "paste", "snap", "compact",
    "repack", "repair", "simplify", "transform", "weld", "fork", "grant",
    "select", "deselect", "swap", "paint", "give", "possess", "grow", "fill",
    "unassign", "insert", "mute", "isolate", "define", "cluster", "fracture",
    "plane", "pack", "break", "claim", "release", "despawn", "map", "unlink",
    "unbind", "unmute", "unload",
}
READONLY_VERBS = {
    "get", "list", "find", "is", "has", "can", "read", "query", "inspect",
    "count", "search", "diff", "compare", "evaluate", "dump", "discover",
    "does", "poll", "audit", "check", "validate", "analyze", "detect",
    "resolve", "scan", "describe", "profile", "lint", "suggest", "export",
    "capture", "snapshot", "verify", "editor", "frame", "print", "show",
    "high", "gather", "trace",
}
# Everything else (run/start/stop/play/pause/load/execute/call/trigger/...) is
# AMBIGUOUS: a likely side effect, but not necessarily an asset write.

# --- patterns (label, regex). The order sets the priority within each list. --- #
HONEST_PATTERNS = [
    ("outcome-response", re.compile(r"\bMakeOutcomeReply\s*\(")),
    ("saved-response", re.compile(r"\bMakeSavedReply\s*\(")),
    ("verified-response", re.compile(r"\bMakeVerifiedReply\s*\(")),
    ("verification-evidence", re.compile(r"\bAddVerificationEvidence\s*\(")),
    # A `*verified` field published with a COMPUTED boolean (not the literal false,
    # which is the honest admission and falls under DECLARED).
    ("verified-field",
     re.compile(r'Set(?:Bool|String)Field\s*\(\s*TEXT\("\w*verif\w*"\)\s*,\s*(?!false\s*\))')),
]
# Calling the honest helper with a constant is NOT honesty: it is detected separately
# and downgrades the class to hardcoded_success.
OUTCOME_LITERAL_TRUE = re.compile(
    r"Make(?:Outcome|Verified|Saved)Reply\s*\([^;]{0,400}?,\s*true\s*[,)]")

DECLARED_PATTERNS = [
    ("verification-enum-honest",
     re.compile(r"EMcpVerification::(?:NotVerifiable|NotAttempted|Failed)")),
    ("verified-false-field",
     re.compile(r'SetBoolField\s*\(\s*TEXT\("\w*verif\w*"\)\s*,\s*false\s*\)')),
    ("note-field",
     re.compile(r'Set\w+Field\s*\(\s*TEXT\("(?:note|notes|warning|warnings|caveat|'
                r'verification|verification_note|not_verified|unverified|best_effort)"\)')),
    ("prose-unverifiable",
     re.compile(r"cannot (?:be )?verif\w+|not verifiable|unverifiable|no read-?back|"
                r"cannot confirm|no way to (?:confirm|verify)|best[- ]effort|"
                r"we do not verify|not verified here", re.I)),
]
SUCCESS_PATTERNS = [
    ("success-response", re.compile(r"\bMakeOkReply\s*\(")),
    ("success-true-field",
     re.compile(r'SetBoolField\s*\(\s*TEXT\("success"\)\s*,\s*true\s*\)')),
]
# Third success path, the MOST invisible one: returning the raw payload. The bridge
# (UEAgentBridge.cpp, "No explicit success field -- treat the whole ResultJson as
# the result") wraps it in `status:"success"` UNCONDITIONALLY. It is an unconditional
# success:true that never mentions the word `success`.
RAW_PAYLOAD_DECL_RE = re.compile(
    r'TSharedPtr\s*<\s*FJsonObject\s*>\s*(\w+)\s*=\s*([^;]{0,240});')


def find_raw_payload_return(body: str, start_line: int):
    """(evidence, is_delegation) for the pattern `TSharedPtr<FJsonObject> X = ...; return X;`.

    If the initializer is a `McpCommandFamilies::Run(...)`, the payload was produced by
    ANOTHER handler: that is delegation, not a success fabricated here. A declaration
    inside an `if (...)` condition is the guard idiom -- `if (TSharedPtr<FJsonObject>
    Error = Check(...)) { return Error; }` returns the helper's error reply -- and is skipped.
    """
    decls = []
    for m in RAW_PAYLOAD_DECL_RE.finditer(body):
        if not re.search(r'\bif\s*\(\s*(?:const\s+)?$', body[max(0, m.start() - 24):m.start()]):
            decls.append(m.groups())
    for name, init in dict.fromkeys(decls):
        m = re.search(r'\breturn\s+' + re.escape(name) + r'\s*;', body)
        if not m:
            continue
        line = start_line + body[:m.start()].count("\n")
        if DELEGATE_RE.search(init):
            return ({"label": "delegated-payload", "line": line,
                     "match": f"return {name};  (payload from another command)"}, True)
        return ({"label": "raw-payload-return", "line": line,
                 "match": f"return {name};  (the bridge wraps it in status:success)"}, False)
    return None, False
DELEGATE_RE = re.compile(r"\bMcpCommandFamilies::Run\s*\(")

# Affirmative boolean fields written by hand: "yes, I did it".
CLAIM_FIELD_RE = re.compile(
    r'SetBoolField\s*\(\s*TEXT\("(applied|success|ok|done|changed|updated|set|saved|'
    r'created|added|removed|deleted|enabled|disabled|assigned|attached|compiled|'
    r'modified|written|applied_to_cdo)"\)\s*,\s*true\s*\)')
# Any field of the response, to cross-check against the input params (echo).
RESP_FIELD_RE = re.compile(r'Set\w+Field\s*\(\s*TEXT\("([^"]+)"\)')

# One-line forward to a helper in the SAME file, by its bare or namespace-qualified name:
#   return HandleRemoveTypedAnimAsset<UAnimBlueprint>(Params, TEXT("..."));
#   return Impl::MapKeyOnce(Context, Action, Key, Reply, TEXT("..."));
# The body located by `parse_file` then holds NO outcome signal (the helper emits it).
# Without following it, such commands fall into `no_outcome_signal` -- a bucket that
# hides honest and lying handlers alike.
FORWARD_RE = re.compile(r'\breturn\s+(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*(?:<[^;{}()\n]*>)?\s*\(')
_MAX_HELPER_BODY = 40000   # bound: an unbalanced brace must not swallow half the file


def _brace_body(text: str, brace: int) -> str:
    depth = 0
    limit = min(len(text), brace + _MAX_HELPER_BODY)
    for j in range(brace, limit):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[brace:j + 1]
    return text[brace:limit]


def find_helper_body(text: str, name: str):
    """(body, start_line) of the DEFINITION of `name` in `text`, or None.

    A definition is a `name(...)` whose closing `)` is followed by `{`; a call
    (`name(...);`) is skipped. Accepts the form with template arguments.
    """
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*(?:<[^;{}()\n]*>)?\s*\(', text):
        depth, j = 0, m.end() - 1
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
        if k < len(text) and text[k] == "{":
            return _brace_body(text, k), text[:k].count("\n") + 1
    return None


def verb_bucket(cmd: str) -> str:
    verb = cmd.split("_", 1)[0]
    if verb in MUTATING_VERBS:
        return "mutating"
    if verb in READONLY_VERBS:
        return "readonly"
    return "ambiguous"


def collect(patterns, body: str, start_line: int):
    hits = []
    for label, rx in patterns:
        m = rx.search(body)
        if m:
            line = start_line + body[:m.start()].count("\n")
            hits.append({"label": label, "line": line, "match": " ".join(m.group(0).split())[:64]})
    return hits


def classify(body: str, start_line: int, param_names: set[str]):
    """Returns (klass, evidence, flags)."""
    honest = collect(HONEST_PATTERNS, body, start_line)
    declared = collect(DECLARED_PATTERNS, body, start_line)
    success = collect(SUCCESS_PATTERNS, body, start_line)
    raw, raw_is_delegation = find_raw_payload_return(body, start_line)
    if raw and not raw_is_delegation:
        success = success + [raw]

    flags: list[str] = []
    m_lit = OUTCOME_LITERAL_TRUE.search(body)
    if m_lit:
        flags.append("outcome-literal-true")
        honest = [h for h in honest
                  if h["label"] not in ("outcome-response", "verified-response")]

    claim_fields = sorted({m.group(1) for m in CLAIM_FIELD_RE.finditer(body)})
    if claim_fields:
        flags.append("literal-true:" + ",".join(claim_fields))

    resp_fields = {m.group(1) for m in RESP_FIELD_RE.finditer(body)}
    echoed = sorted(resp_fields & param_names)
    if echoed:
        flags.append("echo:" + ",".join(echoed))

    delegates = bool(DELEGATE_RE.search(body)) or raw_is_delegation
    if raw_is_delegation:
        ev_delegation = [raw]
    else:
        ev_delegation = []

    if honest:
        klass = "verified"
        if success:
            flags.append("mixed-success-paths")
    elif declared:
        klass = "declared_unverifiable"
    elif success:
        klass = "hardcoded_success"
    elif delegates:
        klass = "delegated"
    else:
        klass = "no_outcome_signal"

    if delegates and klass != "delegated":
        flags.append("also-delegates")
    return klass, honest + declared + success + ev_delegation, flags


def follow_forwarders(text: str, body: str, klass: str, ev: list, flags: list,
                      param_names: set[str], max_helpers: int = 3):
    """Re-classify a forwarding handler using the body of the helper it forwards to.

    Only called when the handler's own body emitted NO outcome signal. One level deep
    and at most `max_helpers` candidates: it is a tie-breaker, not an interpreter.
    Marked with `via-helper:<name>` so the verdict stays auditable (the evidence
    points at the helper's line, not the handler's).
    """
    for name in list(dict.fromkeys(FORWARD_RE.findall(body)))[:max_helpers]:
        found = find_helper_body(text, name)
        if not found:
            continue
        hbody, hstart = found
        if hbody == body:                      # found itself
            continue
        k2, ev2, flags2 = classify(hbody, hstart, param_names)
        if k2 == "no_outcome_signal":
            continue
        return k2, ev2, flags + [f"via-helper:{name}"] + [
            f for f in flags2 if f not in flags]
    return klass, ev, flags


# The object a handler replies with: the first argument of a reply maker.
REPLY_ARG_RE = re.compile(r'\bMake(?:Ok|Saved|Outcome|Verified)Reply\s*\(\s*(\w+)\s*[,)]')


def follow_reply_helpers(text: str, body: str, klass: str, ev: list, flags: list,
                         param_names: set[str], max_helpers: int = 4):
    """Add the evidence of the same-file helpers that BUILD the reply object.

    A helper that receives the reply object (`Fill(Data, ...)`) or returns it
    (`Data = Build(...)`) writes fields the handler replies with, so a `note` or a
    `verified` field it sets is the handler's. Only its honest and declared evidence
    counts: its own success paths are not the handler's. Marked `via-reply-helper:<name>`.
    """
    if klass not in ("hardcoded_success", "declared_unverifiable"):
        return klass, ev, flags
    names = []
    for reply in dict.fromkeys(REPLY_ARG_RE.findall(body)):
        call = (r'\b([A-Za-z_]\w*)\s*(?:<[^;{}()\n]*>)?\s*\([^;]*?\b' + re.escape(reply) + r'\b[^;]*?\)\s*;'
                r'|\b' + re.escape(reply) + r'\s*=\s*([A-Za-z_]\w*)\s*(?:<[^;{}()\n]*>)?\s*\(')
        for m in re.finditer(call, body):
            name = m.group(1) or m.group(2)
            if name and not name.startswith(("Make", "Set", "Get", "Try", "Has")) and name not in names:
                names.append(name)
    for name in names[:max_helpers]:
        found = find_helper_body(text, name)
        if not found or found[0] == body:
            continue
        hbody, hstart = found
        honest = collect(HONEST_PATTERNS, hbody, hstart)
        declared = collect(DECLARED_PATTERNS, hbody, hstart)
        if honest:
            return "verified", honest + ev, flags + [f"via-reply-helper:{name}"]
        if declared and klass == "hardcoded_success":
            klass, ev, flags = "declared_unverifiable", declared + ev, flags + [f"via-reply-helper:{name}"]
    return klass, ev, flags


def load_catalog_params() -> tuple[dict[str, str], dict[str, set[str]]]:
    """(cmd -> domain, cmd -> {param names}) from command_catalog.json."""
    data = json.loads(arm.CATALOG.read_text(encoding="utf-8"))
    cmd2domain: dict[str, str] = {}
    cmd2params: dict[str, set[str]] = {}
    for dom in data.get("domains", []):
        for cmd in dom.get("commands", []):
            cmd2domain[cmd["name"]] = dom["domain"]
            cmd2params[cmd["name"]] = {p["name"] for p in cmd.get("params", [])}
    return cmd2domain, cmd2params


def helper_usage_counts() -> dict[str, int]:
    """Raw occurrences of each response helper across Private/ (context).

    Includes the DEFINITION in McpCommonUtils.* -- it is not a count of pure call
    sites; it is reported as an order of magnitude, not an exact number of calls.
    """
    counts = Counter()
    needles = ("MakeOkReply", "MakeOutcomeReply", "MakeSavedReply",
               "MakeVerifiedReply", "AddVerificationEvidence",
               "MakeErrorReply")
    for path in PRIVATE_DIR.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for n in needles:
            c = text.count(n)
            if c:
                counts[n] += c
    return dict(counts)


def run_audit():
    """Reusable core. Returns (records, counts, buckets, unmapped)."""
    cmd2domain, cmd2params = load_catalog_params()
    records: dict[str, dict] = {}
    seen: set[str] = set()
    text_of: dict[pathlib.Path, str] = {}
    for path in sorted(CMDS_DIR.rglob("*.cpp")):
        bodies, dispatch = arm.parse_file(path)
        # Raw file text (comments included: honest prose counts as a signal), needed
        # to follow a one-line forward to its helper.
        text_of[path] = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(PROJECT).as_posix()
        for cmd, fn in dispatch:
            if cmd in seen:
                continue          # dispatch collision: audit_runtime_mode.py reports it
            seen.add(cmd)
            bucket = verb_bucket(cmd)
            entry = bodies.get(fn)
            if entry is None:
                records[cmd] = {
                    "cmd": cmd, "domain": cmd2domain.get(cmd, "?"), "bucket": bucket,
                    "klass": "no_outcome_signal", "file": rel, "fn": fn,
                    "evidence": [], "flags": ["body-not-located"]}
                continue
            body, start = entry
            klass, ev, flags = classify(body, start, cmd2params.get(cmd, set()))
            if klass == "no_outcome_signal":
                klass, ev, flags = follow_forwarders(
                    text_of[path], body, klass, ev, flags, cmd2params.get(cmd, set()))
            klass, ev, flags = follow_reply_helpers(
                text_of[path], body, klass, ev, flags, cmd2params.get(cmd, set()))
            records[cmd] = {
                "cmd": cmd, "domain": cmd2domain.get(cmd, "?"), "bucket": bucket,
                "klass": klass, "file": rel, "fn": fn,
                "evidence": ev, "flags": flags}
    unmapped = sorted(c for c in cmd2domain if c not in records)
    mut = [r for r in records.values() if r["bucket"] == "mutating"]
    counts = Counter(r["klass"] for r in mut)
    buckets = Counter(r["bucket"] for r in records.values())
    return records, counts, buckets, unmapped


def ev_str(evidence):
    return ", ".join(f"`{e['label']}`@{e['line']}" for e in evidence) or "—"


def flag_str(flags):
    return ", ".join(f"`{f}`" for f in flags) or "—"


def build_markdown(records, counts, buckets, unmapped, domain_filter):
    recs = [r for r in records.values() if r["bucket"] == "mutating"]
    if domain_filter:
        recs = [r for r in recs if r["domain"].lower() == domain_filter.lower()]
        # The counters MUST follow the filter: showing the global total above
        # filtered tables would be exactly the kind of false data this audit
        # hunts for.
        counts = Counter(r["klass"] for r in recs)
        buckets = Counter(r["bucket"] for r in records.values()
                          if r["domain"].lower() == domain_filter.lower())
    by_class = defaultdict(list)
    for r in recs:
        by_class[r["klass"]].append(r)

    out = []
    out.append("# Outcome honesty of the mutating MCP commands\n")
    out.append("> **AUTO-GENERATED** — do not edit by hand. Regenerate with "
               "`python Tools/audit_outcome_honesty.py --md <file>`.\n")
    out.append("> Heuristic **with cited evidence** (pattern@line), like "
               "`audit_runtime_mode.py`. Read-only tools that return wrong data are "
               "invisible to an outcome audit; this covers the much larger population "
               "of mutating handlers whose `success:true` is a constant.\n")
    if domain_filter:
        out.append(f"**Domain filter:** `{domain_filter}`\n")

    out.append("## Audited population\n")
    out.append("| Verb | Commands | Audited |")
    out.append("|---|---:|---|")
    out.append(f"| mutating | {buckets.get('mutating', 0)} | yes |")
    out.append(f"| read-only | {buckets.get('readonly', 0)} | no (no outcome to contrast) |")
    out.append(f"| ambiguous | {buckets.get('ambiguous', 0)} | no (effect not guaranteed) |")
    out.append(f"| **total with dispatch** | **{sum(buckets.values())}** | |\n")

    total_mut = sum(counts.values())
    out.append("## Verdict on the mutating commands\n")
    out.append("| Class | Commands | % |")
    out.append("|---|---:|---:|")
    for k in ("verified", "declared_unverifiable", "hardcoded_success",
              "delegated", "no_outcome_signal"):
        n = counts.get(k, 0)
        pct = (100.0 * n / total_mut) if total_mut else 0.0
        out.append(f"| {k} | {n} | {pct:.1f}% |")
    out.append(f"| **total** | **{total_mut}** | |\n")

    def table(klass, title, note=""):
        rows = sorted(by_class.get(klass, []), key=lambda r: (r["domain"], r["cmd"]))
        out.append(f"## {title} — {len(rows)}\n")
        if note:
            out.append(note + "\n")
        if not rows:
            out.append("_(none)_\n")
            return
        out.append("| Command | Domain | Evidence | Flags | Handler |")
        out.append("|---|---|---|---|---|")
        for r in rows:
            out.append(f"| `{r['cmd']}` | {r['domain']} | {ev_str(r['evidence'])} | "
                       f"{flag_str(r['flags'])} | {r['file'].split('/')[-1]}::{r['fn']} |")
        out.append("")

    table("verified", "verified (the outcome comes from a measurement)",
          "They use `MakeOutcomeReply`/`MakeVerifiedReply`/`AddVerificationEvidence` "
          "or publish a computed `*verified`.")
    table("declared_unverifiable", "declared_unverifiable (they admit the limit)",
          "No pretending: the response declares that the effect could not be checked.")
    table("hardcoded_success", "hardcoded_success (the `true` is a constant)",
          "Debt. `echo:` marks a response that re-emits the input (the "
          "`set_actor_team` pattern); `literal-true:` marks a claim written by hand.")
    table("delegated", "delegated (they forward to another handler)",
          "The outcome comes from the target handler, which is audited on its own.")
    table("no_outcome_signal", "no_outcome_signal (no success emission detected)",
          "Body not located, or no `MakeOkReply`. Review by hand.")

    if unmapped and not domain_filter:
        out.append(f"## No locatable dispatch — {len(unmapped)}\n")
        out.append("Catalog commands this scan could not attribute to a body "
                   "(this list should be empty).\n")
        for c in unmapped:
            out.append(f"- `{c}`")
        out.append("")

    out.append("## Context: raw usage of the response helpers\n")
    out.append("Text occurrences across `Private/**/*.cpp` (including the definition "
               "in `McpCommonUtils.cpp`): an order of magnitude, not exact call sites.\n")
    out.append("| Helper | Occurrences |")
    out.append("|---|---:|")
    for k, v in sorted(helper_usage_counts().items(), key=lambda kv: -kv[1]):
        out.append(f"| `{k}` | {v} |")
    out.append("")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Audit the outcome honesty of the mutating MCP commands.")
    ap.add_argument("--json", metavar="PATH", help="write the structured report as JSON")
    ap.add_argument("--md", metavar="PATH", help="write the Markdown report to a file")
    ap.add_argument("--domain", metavar="NAME", help="filter to a single domain")
    ap.add_argument("--cmd", metavar="NAME", help="record of a single command (with evidence)")
    ap.add_argument("--summary", action="store_true", help="counters only")
    args = ap.parse_args(argv)

    if not arm.CATALOG.exists():
        sys.exit(f"ERROR: catalog not found at {arm.CATALOG}")
    if not CMDS_DIR.exists():
        sys.exit(f"ERROR: Commands tree not found at {CMDS_DIR}")

    records, counts, buckets, unmapped = run_audit()

    if args.cmd:
        r = records.get(args.cmd)
        if r is None:
            sys.exit(f"'{args.cmd}' has no locatable dispatch in the scanned C++")
        print(json.dumps(r, indent=2, ensure_ascii=False))
        return

    if args.summary:
        total_mut = sum(counts.values())
        print(f"commands with dispatch  : {sum(buckets.values())}")
        print(f"  mutating              : {buckets.get('mutating', 0)}")
        print(f"  read-only             : {buckets.get('readonly', 0)}")
        print(f"  ambiguous             : {buckets.get('ambiguous', 0)}")
        print(f"mutating audited        : {total_mut}")
        for k in ("verified", "declared_unverifiable", "hardcoded_success",
                  "delegated", "no_outcome_signal"):
            print(f"  {k:<22}: {counts.get(k, 0)}")
        if unmapped:
            print(f"no locatable dispatch   : {len(unmapped)}")
        return

    md = build_markdown(records, counts, buckets, unmapped, args.domain)

    if args.json:
        payload = {
            "buckets": dict(buckets),
            "summary_mutating": dict(counts),
            "commands": sorted(records.values(),
                               key=lambda r: (r["bucket"], r["klass"], r["domain"], r["cmd"])),
            "unmapped": unmapped,
            "helper_usage": helper_usage_counts(),
        }
        pathlib.Path(args.json).write_text(
            json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"JSON written to {args.json}", file=sys.stderr)
    if args.md:
        pathlib.Path(args.md).write_text(md, encoding="utf-8")
        print(f"Markdown written to {args.md}", file=sys.stderr)
    if not args.md:
        print(md)


if __name__ == "__main__":
    main()
