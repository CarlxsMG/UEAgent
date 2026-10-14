"""Re-baseline the PROSE of the contract vectors after a deliberate text change.

The contract self-test (`mcp.contract.selftest`) compares whole outputs, and part of each output is
prose (a refusal `message`, a readback `note`). When those texts are rewritten on purpose, the
vectors must follow -- but ONLY the prose: a changed verdict, flag, count or key is a contract
change and must stay a failure.

Reads the report the self-test wrote next to each vector file (`<file>.result.json`), and for every
failure decides:
  - prose-only: expected and actual are identical once every PROSE leaf is blanked
    -> with --apply, the prose leaves of the vector are replaced by the actual ones;
  - anything else: reported, never touched.

--strings widens "prose" to EVERY string leaf (e.g. a translated `stored` value such as
"3 errors"), but only when the structure, numbers, booleans and keys are identical. It prints each
distinct old -> new string change outside the prose keys, so the change can be reviewed before it
is applied with --strings --apply.

Usage:
  python Tools/rebaseline_contract_prose.py [--strings] [--apply]
Then re-run the self-test: it must pass with no failures.
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import _paths  # noqa: E402

TESTS_DIR = _paths.PLUGIN_ROOT_P / "Resources" / "ContractTests"
# Keys whose string values are prose, not contract.
PROSE_KEYS = {"note", "message", "detail", "hint", "error", "reason", "warning", "summary_text"}


def decode(value):
    """Decode the vectors' JSON encodings as the C++ reads them: {"__set__": [...]} -> list."""
    if isinstance(value, dict):
        if len(value) == 1 and isinstance(value.get("__set__"), list):
            return [decode(v) for v in value["__set__"]]
        return {k: decode(v) for k, v in value.items()}
    if isinstance(value, list):
        return [decode(v) for v in value]
    return value


def skeleton(value, is_text, key=None):
    if isinstance(value, dict):
        return {k: skeleton(v, is_text, k) for k, v in value.items()}
    if isinstance(value, list):
        return [skeleton(v, is_text, key) for v in value]
    if isinstance(value, str) and is_text(key):
        return "<text>"
    return value


def patch_text(expected, actual, is_text, key=None):
    """Copy the text leaves of `actual` into `expected`, keeping expected's encoding."""
    if isinstance(expected, dict):
        if len(expected) == 1 and isinstance(expected.get("__set__"), list):
            return {"__set__": patch_text(expected["__set__"], actual, is_text, key)}
        return {k: patch_text(v, actual[k], is_text, k) for k, v in expected.items()}
    if isinstance(expected, list):
        return [patch_text(e, a, is_text, key) for e, a in zip(expected, actual)]
    if isinstance(expected, str) and is_text(key):
        return actual
    return expected


def value_changes(expected, actual, key=None, out=None):
    """Distinct (key, old, new) string changes outside the prose keys."""
    out = set() if out is None else out
    if isinstance(expected, dict) and isinstance(actual, dict):
        for k, v in expected.items():
            if k in actual:
                value_changes(v, actual[k], k, out)
    elif isinstance(expected, list) and isinstance(actual, list):
        for e, a in zip(expected, actual):
            value_changes(e, a, key, out)
    elif isinstance(expected, str) and isinstance(actual, str) and expected != actual and key not in PROSE_KEYS:
        out.add((key, expected, actual))
    return out


def rebaseline_readback(apply: bool, is_text, changes: set) -> tuple[int, int]:
    vectors_path = TESTS_DIR / "readback_vectors.json"
    report = json.loads((TESTS_DIR / "readback_vectors.json.result.json").read_text(encoding="utf-8"))
    vectors = json.loads(vectors_path.read_text(encoding="utf-8"))
    text_only = logic = 0
    for failure in report.get("failures", []):
        index = failure.get("index")
        have = failure.get("have")
        record = vectors["records"][index] if isinstance(index, int) else None
        same_shape = (record is not None and have is not None
                      and not failure.get("probe_mismatch") and not failure.get("state_mismatch")
                      and skeleton(decode(record["expect"]), is_text) == skeleton(have, is_text))
        if same_shape:
            text_only += 1
            value_changes(decode(record["expect"]), have, out=changes)
            if apply:
                record["expect"] = patch_text(record["expect"], have, is_text)
        else:
            logic += 1
            print(f"  LOGIC readback #{index} {failure.get('command')}: {failure.get('why', '')[:300]}")
    if apply and text_only:
        vectors_path.write_bytes(json.dumps(vectors, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    return text_only, logic


def rebaseline_refuse(apply: bool) -> tuple[int, int]:
    vectors_path = TESTS_DIR / "refuse_vectors.json"
    raw = vectors_path.read_text(encoding="utf-8")
    vectors = json.loads(raw)
    report = json.loads((TESTS_DIR / "refuse_vectors.json.result.json").read_text(encoding="utf-8"))
    cases = {case["id"]: case for case in vectors["cases"]}
    prose = logic = 0
    for failure in report.get("failures", []):
        case = cases.get(failure.get("case"))
        step_index = failure.get("step")
        if (case is not None and isinstance(step_index, int)
                and failure.get("expected_refused") == failure.get("got_refused")
                and not failure.get("probe_diff")):
            prose += 1
            if apply:
                expect = case["steps"][step_index]["expect"]
                expect.pop("message_prefix", None)
                expect["message"] = failure.get("got_message") or None
        else:
            logic += 1
            print(f"  LOGIC refuse {failure.get('case')} step {step_index}: refused {failure.get('expected_refused')}"
                  f" -> {failure.get('got_refused')} {failure.get('probe_diff', '')[:200]}")
    if apply and prose:
        compact = raw.count("\n") <= 2
        ascii_only = raw.isascii()
        text = (json.dumps(vectors, ensure_ascii=ascii_only, separators=(",", ":")) if compact
                else json.dumps(vectors, ensure_ascii=ascii_only, indent=1))
        vectors_path.write_bytes(text.encode("utf-8"))
    return prose, logic


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apply", action="store_true", help="rewrite the text of text-only failures")
    parser.add_argument("--strings", action="store_true",
                        help="treat every string leaf as text (review the printed value changes first)")
    args = parser.parse_args()
    is_text = (lambda key: True) if args.strings else (lambda key: key in PROSE_KEYS)
    changes = set()
    text_only, logic_rb = rebaseline_readback(args.apply, is_text, changes)
    print(f"readback: {text_only} text-only failure(s){' re-baselined' if args.apply else ''}, {logic_rb} contract failure(s)")
    if args.strings and changes:
        print(f"  {len(changes)} distinct value change(s) outside the prose keys:")
        for key, old, new in sorted(changes, key=lambda c: (str(c[0]), c[1])):
            print(f"    [{key}] {old!r} -> {new!r}")
    prose, logic_rf = rebaseline_refuse(args.apply)
    print(f"refuse: {prose} prose-only failure(s){' re-baselined' if args.apply else ''}, {logic_rf} contract failure(s)")
    return 1 if (logic_rb or logic_rf) else 0


if __name__ == "__main__":
    sys.exit(main())
