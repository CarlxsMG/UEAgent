"""
smoke_test.py -- end-to-end checks of a running UEAgent server, over HTTP, as a client sees it.

Usage:
  python Tools/smoke_test.py
      read-only checks against the editor already serving http://127.0.0.1:55561/mcp
  python Tools/smoke_test.py --launch D:/Tests/Scratch/Scratch.uproject --engine "C:/Program Files/Epic Games/UE_5.8"
      starts a headless editor on that project, runs every check, edits included, and closes it

The edit checks spawn actors, undo them and load a new level, so they only run in an editor this
script started: use a throwaway project for --launch. Checks:
  - protocol: initialize (with the agent instructions), ping, the full and the compact tool list;
  - the meta-tools: find_tools, describe_tool (the same schema as the full list), call_tool (the
    same answer as a direct call), the closest names for an unknown tool;
  - edits: one undo step per call, an execute_batch as one step, undo/redo and the undo-state tools,
    mcp.UndoStepPerCall, and a level load in the middle of a call.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "Resources" / "command_catalog.json"


class Session:
    def __init__(self, url: str) -> None:
        self.url = url
        self.passed = 0
        self.failed = 0
        self.next_id = 0
        self.full_list: dict = {}

    def check(self, name: str, ok: bool, detail: object = "") -> None:
        if ok:
            self.passed += 1
            print(f"  OK   {name}")
        else:
            self.failed += 1
            print(f"  FAIL {name} :: {str(detail)[:400]}")

    def rpc(self, method: str, params: dict | None = None, query: str = "") -> dict:
        self.next_id += 1
        body = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            body["params"] = params
        request = urllib.request.Request(self.url + query, data=json.dumps(body).encode(), method="POST", headers={
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(request, timeout=300) as response:
            return json.loads(response.read().decode("utf-8"))

    def call(self, tool: str, arguments: object = None) -> tuple[bool, dict]:
        """tools/call -> (isError, the JSON the tool answered)."""
        params = {"name": tool, "arguments": {} if arguments is None else arguments}
        result = self.rpc("tools/call", params)["result"]
        text = result["content"][0]["text"]
        try:
            payload = json.loads(text)
        except ValueError:
            payload = {"_text": text}
        return bool(result.get("isError")), payload if isinstance(payload, dict) else {"_value": payload}


def wait_for_server(session: Session, seconds: int) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            session.rpc("ping")
            return True
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(2)
    return False


def port_is_open(url: str) -> bool:
    host_port = url.split("://", 1)[1].split("/", 1)[0]
    host, _, port = host_port.partition(":")
    with socket.socket() as probe:
        probe.settimeout(1)
        return probe.connect_ex((host, int(port or 80))) == 0


def check_protocol(s: Session, catalog: dict) -> None:
    print("== protocol")
    total = sum(len(d["commands"]) for d in catalog["domains"])
    init = s.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "smoke-test", "version": "1"}})["result"]
    s.check("initialize names the server", init.get("serverInfo", {}).get("name") == "UEAgent", init)
    s.check("initialize carries the agent instructions", "readback" in init.get("instructions", ""), init)
    s.check("ping answers", s.rpc("ping").get("result") == {}, "")
    full = s.rpc("tools/list", query="?tools=full")["result"]["tools"]
    s.check(f"the full list has the catalog's {total} tools", len(full) == total, len(full))
    compact = s.rpc("tools/list", query="?tools=compact")["result"]["tools"]
    s.check("the compact list has the three meta-tools",
            [t["name"] for t in compact] == ["find_tools", "describe_tool", "call_tool"], [t["name"] for t in compact])
    compact_init = s.rpc("initialize", {"protocolVersion": "2025-06-18"}, query="?tools=compact")["result"]
    s.check("compact initialize explains the meta-tools", "find_tools" in compact_init.get("instructions", ""), "")
    s.check("an unknown method is -32601", s.rpc("no/such/method").get("error", {}).get("code") == -32601, "")
    s.full_list = {t["name"]: t for t in full}


def check_meta_tools(s: Session, catalog: dict) -> None:
    print("== meta-tools")
    is_error, found = s.call("find_tools", {})
    s.check("find_tools lists the domains", not is_error and len(found.get("domains", [])) == len(catalog["domains"]), found)
    is_error, found = s.call("find_tools", {"query": "spawn actor"})
    names = [t["name"] for t in found.get("tools", [])]
    s.check("find_tools ranks spawn_actor first for 'spawn actor'", names[:1] == ["spawn_actor"], names[:5])
    is_error, described = s.call("describe_tool", {"name": "spawn_actor"})
    s.check("describe_tool gives the full list's schema",
            not is_error and described.get("inputSchema") == s.full_list["spawn_actor"]["inputSchema"], described)
    _, direct = s.call("get_project_info")
    _, through = s.call("call_tool", {"name": "get_project_info", "arguments": {}})
    s.check("call_tool answers as the direct call", direct == through, (direct, through))
    _, as_string = s.call("call_tool", {"name": "get_project_info", "arguments": "{}"})
    s.check("call_tool accepts arguments as a JSON string", as_string == direct, as_string)
    is_error, unknown = s.call("spawn_actr")
    s.check("an unknown tool names the closest ones", is_error and "spawn_actor" in unknown.get("_text", ""), unknown)


def newest_title(s: Session) -> tuple[str | None, int]:
    _, history = s.call("get_transaction_history", {"max_entries": 100})
    live = [e for e in history.get("entries", []) if not e.get("is_undone")]
    return (live[0]["title"] if live else None), len(live)  # newest first


def actor_names(s: Session) -> set[str]:
    _, level = s.call("get_actors_in_level", {"max_results": 10000})
    return {a.get("name") for a in level.get("actors", [])}


def check_edits(s: Session) -> None:
    print("== one undo step per call")
    _, before = newest_title(s)
    s.call("spawn_actor", {"type": "PointLight", "name": "SmokeA"})
    title, count = newest_title(s)
    s.check("a call is one step named after the tool", title == "UEAgent: spawn_actor" and count == before + 1, (title, count))
    s.call("get_project_info")
    s.check("a read leaves no step", newest_title(s) == (title, count), newest_title(s))
    s.call("spawn_actor", {"type": "NoSuchType", "name": "SmokeBad"})
    s.check("a failed call leaves no step", newest_title(s)[1] == count, newest_title(s))
    s.call("execute_batch", {"commands": [{"command_type": "spawn_actor", "params": {"type": "PointLight", "name": "SmokeB"}},
                                  {"command_type": "spawn_actor", "params": {"type": "PointLight", "name": "SmokeC"}}]})
    s.check("a batch is one step", newest_title(s) == ("UEAgent: execute_batch", count + 1), newest_title(s))
    _, can = s.call("get_undo_state")
    s.check("get_undo_state names what undo reverts", can.get("can_undo") is True and can.get("next_undo") == "UEAgent: execute_batch", can)
    _, undone = s.call("undo")
    _, state = s.call("get_undo_state")
    names = actor_names(s)
    s.check("undo reverts the whole batch", undone.get("applied") is True and not {"SmokeB", "SmokeC"} & names
            and "SmokeA" in names, (undone, sorted(n for n in names if n and n.startswith("Smoke"))))
    s.check("get_undo_state counts the redo side", state.get("redo_count") == 1 and state.get("next_redo") == "UEAgent: execute_batch", state)
    _, redone = s.call("redo")
    s.check("redo restores it", redone.get("applied") is True and {"SmokeB", "SmokeC"} <= actor_names(s), redone)

    print("== mcp.UndoStepPerCall")
    s.call("execute_console_command", {"command": "mcp.UndoStepPerCall 0"})
    s.call("spawn_actor", {"type": "PointLight", "name": "SmokeD"})
    s.check("0 leaves undo to the tool", newest_title(s)[0] != "UEAgent: spawn_actor", newest_title(s))
    s.call("execute_console_command", {"command": "mcp.UndoStepPerCall 1"})
    s.call("spawn_actor", {"type": "PointLight", "name": "SmokeE"})
    s.check("1 brings the step back", newest_title(s)[0] == "UEAgent: spawn_actor", newest_title(s))

    print("== a level load in the middle of a call")
    is_error, loaded = s.call("load_level", {"default_template": True})
    s.check("a level load runs inside its undo step", not is_error and loaded.get("success") is not False, loaded)
    s.call("spawn_actor", {"type": "PointLight", "name": "SmokeF"})
    s.check("the buffer works after the reset", newest_title(s) == ("UEAgent: spawn_actor", 1), newest_title(s))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://127.0.0.1:55561/mcp", help="The MCP endpoint")
    parser.add_argument("--launch", type=Path, help="A throwaway .uproject: start a headless editor on it")
    parser.add_argument("--engine", type=Path, help="Unreal Engine root (with --launch)")
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    session = Session(args.url)
    editor = None
    if args.launch:
        if not args.engine:
            parser.error("--launch needs --engine")
        if port_is_open(args.url):
            print(f"FAIL: something already listens on {args.url}: close that editor first")
            return 1
        binary = args.engine / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
        editor = subprocess.Popen([str(binary), str(args.launch.resolve()), "-unattended", "-nopause", "-nosplash",
                                   "-NullRHI", "-NoSound", "-log=UEAgentSmoke.log"])
        print(f"Started a headless editor (pid {editor.pid}) on {args.launch.name}")
    try:
        if not wait_for_server(session, 600 if editor else 10):
            print(f"FAIL: no UEAgent server answers at {args.url}")
            return 1
        check_protocol(session, catalog)
        check_meta_tools(session, catalog)
        if editor:
            check_edits(session)
        else:
            print("== edit checks skipped: they only run in an editor started with --launch")
    finally:
        if editor:
            editor.kill()
            editor.wait(timeout=120)
    print(f"\n{session.passed} passed, {session.failed} failed")
    return 1 if session.failed else 0


if __name__ == "__main__":
    sys.exit(main())
