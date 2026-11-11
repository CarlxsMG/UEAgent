"""
smoke_test.py -- end-to-end checks of a running UEAgent server, over HTTP, as a client sees it.

Usage:
  python Tools/smoke_test.py
      read-only checks against the editor already serving http://127.0.0.1:55561/mcp
  python Tools/smoke_test.py --launch D:/Tests/Scratch/Scratch.uproject --engine "C:/Program Files/Epic Games/UE_5.8"
      starts a headless editor on that project, runs every check, edits included, and closes it

The edit checks spawn actors, undo them and load a new level, so they only run in an editor this
script started: use a throwaway project for --launch. Checks:
  - protocol: initialize (with the agent instructions), ping, the core (default), full and compact tool lists;
  - the core list: its session, ?domains=, and enable_toolset adding a domain and announcing it over SSE;
  - the meta-tools: find_tools, describe_tool (the same schema as the full list), call_tool (the
    same answer as a direct call), the closest names for an unknown tool;
  - the shipped skills as prompts and resources: one prompt per skill, the tool map as ueagent://guide, every
    listed resource readable, and the errors for an unknown prompt or resource;
  - tests and Play-in-Editor control with nothing running: pause_play_in_editor, cancel_automation_test,
    is_play_in_editor_active's is_paused;
  - edits: one undo step per call, an execute_batch as one step, undo/redo and the undo-state tools,
    mcp.UndoStepPerCall, undo barriers set and removed, an impulse on a body that does not simulate, and a
    level load in the middle of a call;
  - images: render_widget_preview answers with a PNG image block after its text, directly, through call_tool and
    in execute_batch, and with the text alone for return_image=false and ?images=off. Without rendering (the
    default -NullRHI launch) it checks the text-alone answer and skips the image blocks; --render launches with
    -RenderOffscreen instead, which renders headless on a machine with a GPU, and adds capture_viewport.
  - source control, with no provider enabled: the file status answers enabled=false and checkout_files refuses.
  - the tool permissions (--launch only): a domain and a tool turned off (not listed, not found, refused directly,
    through call_tool and as an execute_batch item, the readback still re-reading with a tool turned off), the
    list_changed notice and the list restored, code execution off; then a second editor started with -McpReadOnly:
    the three lists, and a write refused by every way in while a read and a capture run.

The launched editor serves on the port of --url (-McpHttpPort).
"""

from __future__ import annotations

import argparse
import base64
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
SKILLS = ROOT / "Resources" / "Skills"


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

    def raw(self, method: str, params: dict | None = None, query: str = "", headers: dict | None = None) -> tuple[dict, str]:
        """One request as it travels: (the response headers, lower-case names; the body as text)."""
        self.next_id += 1
        body = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            body["params"] = params
        request = urllib.request.Request(self.url + query, data=json.dumps(body).encode(), method="POST", headers={
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream", **(headers or {})})
        with urllib.request.urlopen(request, timeout=300) as response:
            return {k.lower(): v for k, v in response.headers.items()}, response.read().decode("utf-8")

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
    total = sum(not c.get("retired") for d in catalog["domains"] for c in d["commands"])
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


def sse_messages(text: str) -> list[dict]:
    """The JSON-RPC messages of an SSE body, in order."""
    return [json.loads(line[len("data: "):]) for line in text.splitlines() if line.startswith("data: ")]


def check_core_list(s: Session, catalog: dict) -> None:
    print("== core list")
    meta = ["find_tools", "describe_tool", "call_tool", "enable_toolset"]
    core = [c["name"] for d in catalog["domains"] for c in d["commands"] if c.get("core") and not c.get("retired")]
    water = [c["name"] for d in catalog["domains"] if d["domain"] == "Water" for c in d["commands"]
             if not c.get("retired") and not c.get("core")]
    headers, body = s.raw("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "smoke-test", "version": "1"}})
    init = json.loads(body)["result"]
    session = headers.get("mcp-session-id", "")
    s.check("initialize on the core list opens a session (Mcp-Session-Id)", bool(session), headers)
    s.check("the core list announces listChanged", init.get("capabilities", {}).get("tools", {}).get("listChanged") is True, init)
    s.check("the core instructions name enable_toolset", "enable_toolset" in init.get("instructions", ""), "")
    listed = [t["name"] for t in s.rpc("tools/list")["result"]["tools"]]
    s.check(f"tools/list carries the {len(core)} core tools and the four meta-tools by default", listed == core + meta, listed[-6:])
    listed = [t["name"] for t in s.rpc("tools/list", query="?domains=Water")["result"]["tools"]]
    s.check("?domains=Water adds the Water tools", set(listed) == set(core + meta + water), len(listed))
    all_headers, all_body = s.raw("initialize", {"protocolVersion": "2025-06-18"}, query="?tools=all")
    s.check("?tools=all opens a session and announces listChanged too (the tool permissions can change any list)",
            "mcp-session-id" in all_headers and json.loads(all_body)["result"]["capabilities"]["tools"]["listChanged"] is True, all_headers)

    params = {"name": "enable_toolset", "arguments": {"domains": ["water"]}}
    headers, body = s.raw("tools/call", params, headers={"Mcp-Session-Id": session})
    messages = sse_messages(body) if "text/event-stream" in headers.get("content-type", "") else []
    s.check("enable_toolset answers over SSE, the list_changed notification first",
            [m.get("method") for m in messages[:1]] == ["notifications/tools/list_changed"], body[:300])
    reply = json.loads(messages[-1]["result"]["content"][0]["text"]) if messages else {}
    s.check(f"enable_toolset adds the {len(water)} Water tools", reply.get("tools_added") == len(water), reply)
    headers, body = s.raw("tools/list", headers={"Mcp-Session-Id": session})
    listed = [t["name"] for t in json.loads(body)["result"]["tools"]]
    s.check("the session's tools/list now carries them", set(listed) == set(core + meta + water), len(listed))
    headers, body = s.raw("tools/call", params, headers={"Mcp-Session-Id": session})
    reply = json.loads(json.loads(body)["result"]["content"][0]["text"])
    s.check("enabling it again adds nothing and answers JSON", reply.get("tools_added") == 0 and not reply.get("list_changed"), reply)
    is_error, reply = s.call("enable_toolset", {"domains": ["Water"]})
    s.check("without a session it adds nothing and says how to reach them", not is_error and reply.get("tools_added") == 0
            and "call_tool" in reply.get("note", ""), reply)
    is_error, reply = s.call("enable_toolset", {"domains": ["NoSuchDomain"]})
    s.check("an unknown domain is an error that lists the domains", is_error and "Water" in reply.get("_text", ""), reply)

    # Packs: a few tools for one job, added by name like a domain.
    packs = {p["name"]: [c["name"] for d in catalog["domains"] for c in d["commands"]
                         if c.get("pack") == p["name"] and not c.get("retired")] for p in catalog.get("packs", [])}
    s.check("the catalog has packs, each with tools", bool(packs) and all(packs.values()), packs)
    actors = packs.get("actors", [])
    listed = [t["name"] for t in s.rpc("tools/list", query="?packs=actors")["result"]["tools"]]
    s.check("?packs=actors adds the actors pack", set(listed) == set(core + meta + actors), len(listed))
    listed = [t["name"] for t in s.rpc("tools/list", query="?domains=actors")["result"]["tools"]]
    s.check("?domains= takes a pack name too", set(listed) == set(core + meta + actors), len(listed))
    headers, body = s.raw("tools/call", {"name": "enable_toolset", "arguments": {"packs": ["actors"]}},
                          headers={"Mcp-Session-Id": session})
    messages = sse_messages(body) if "text/event-stream" in headers.get("content-type", "") else []
    reply = json.loads(messages[-1]["result"]["content"][0]["text"]) if messages else {}
    s.check(f"enable_toolset adds the {len(actors)} tools of the actors pack",
            reply.get("tools_added") == len(actors) and [p.get("pack") for p in reply.get("packs", [])] == ["actors"], reply)
    headers, body = s.raw("tools/list", headers={"Mcp-Session-Id": session})
    listed = [t["name"] for t in json.loads(body)["result"]["tools"]]
    s.check("the session's tools/list carries the pack and the domain", set(listed) == set(core + meta + water + actors), len(listed))
    play_and_level = set(packs.get("play", [])) | {c["name"] for d in catalog["domains"] if d["domain"] == "Level"
                                                     for c in d["commands"] if not c.get("retired") and not c.get("core")}
    is_error, reply = s.call("enable_toolset", {"packs": ["play"], "domains": ["Level"]})
    s.check("a pack and a domain together answer both, without a session adding nothing",
            not is_error and [p.get("pack") for p in reply.get("packs", [])] == ["play"]
            and [d.get("domain") for d in reply.get("domains", [])] == ["Level"] and reply.get("tools_added") == 0, reply)
    headers, body = s.raw("initialize", {"protocolVersion": "2025-06-18"})
    other = headers.get("mcp-session-id", "")
    headers, body = s.raw("tools/call", {"name": "enable_toolset", "arguments": {"packs": ["play"], "domains": ["Level"]}},
                          headers={"Mcp-Session-Id": other})
    messages = sse_messages(body) if "text/event-stream" in headers.get("content-type", "") else []
    reply = json.loads(messages[-1]["result"]["content"][0]["text"]) if messages else {}
    s.check(f"a pack's tools inside an enabled domain count once ({len(play_and_level)})",
            reply.get("tools_added") == len(play_and_level), reply)
    is_error, reply = s.call("enable_toolset", {"packs": ["no_such_pack"]})
    s.check("an unknown pack is an error that lists the packs", is_error and "actors" in reply.get("_text", ""), reply)
    is_error, reply = s.call("enable_toolset", {})
    s.check("enable_toolset with nothing to add is an error", is_error, reply)


def check_meta_tools(s: Session, catalog: dict) -> None:
    print("== meta-tools")
    is_error, found = s.call("find_tools", {})
    s.check("find_tools lists the domains", not is_error and len(found.get("domains", [])) == len(catalog["domains"]), found)
    covers = {d["domain"]: d.get("description") for d in catalog["domains"]}
    s.check("find_tools says what each domain covers, as the catalog does",
            all(d.get("description") and d.get("description") == covers.get(d.get("domain")) for d in found.get("domains", [])),
            [d for d in found.get("domains", []) if d.get("description") != covers.get(d.get("domain"))][:3])
    s.check("find_tools lists the packs with their tools",
            [p.get("pack") for p in found.get("packs", [])] == [p["name"] for p in catalog.get("packs", [])]
            and all(p.get("tools") for p in found.get("packs", [])), found.get("packs"))
    core_list = {t["name"]: t for t in s.rpc("tools/list")["result"]["tools"]}
    hints = {name: core_list.get(name, {}).get("annotations", {}) for name in ("find_tools", "describe_tool", "call_tool", "enable_toolset")}
    s.check("the meta-tools carry all four annotations, call_tool the worst of any tool",
            all(len(h) == 4 for h in hints.values()) and hints["call_tool"].get("destructiveHint") is True
            and hints["find_tools"].get("readOnlyHint") is True, hints)
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


def check_cpp_compile(s: Session) -> None:
    print("== C++ compile status")
    is_error, status = s.call("get_cpp_compile_status")
    data = status.get("data", {})
    s.check("get_cpp_compile_status with no compile started answers has_compile false and the Live Coding state",
            not is_error and data.get("has_compile") is False and "available" in data.get("live_coding", {}), status)


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

    print("== undo barriers")
    s.call("remove_undo_barrier", {"all": True})
    _, none = s.call("remove_undo_barrier")
    s.check("removing a barrier that is not there fails", none.get("success") is False
            and none.get("result", {}).get("removed") is False, none)
    _, placed = s.call("set_undo_barrier")
    s.check("set_undo_barrier reads its barrier back", placed.get("success") is True and placed.get("undo_barrier", -1) >= 0, placed)
    _, popped = s.call("remove_undo_barrier")
    s.check("removing a barrier that is there succeeds", popped.get("success") is True
            and popped.get("removed") is True, popped)
    _, cleared = s.call("remove_undo_barrier", {"all": True})
    s.check("all=true with none succeeds with a note", cleared.get("success") is True
            and cleared.get("removed") is False and cleared.get("note"), cleared)

    print("== an impulse on a body that does not simulate")
    s.call("spawn_actor", {"type": "StaticMeshActor", "name": "SmokeImpulse"})
    _, kicked = s.call("add_impulse_to_actor", {"actor_name": "SmokeImpulse", "impulse": [0, 0, 1000]})
    data = kicked.get("data", {})
    s.check("it answers applied=false with a note", kicked.get("success") is True and data.get("applied") is False
            and data.get("simulating_physics") is False and data.get("world") == "editor" and data.get("note"), kicked)

    print("== a level load in the middle of a call")
    is_error, loaded = s.call("load_level", {"default_template": True})
    s.check("a level load runs inside its undo step", not is_error and loaded.get("success") is not False, loaded)
    s.call("spawn_actor", {"type": "PointLight", "name": "SmokeF"})
    s.check("the buffer works after the reset", newest_title(s) == ("UEAgent: spawn_actor", 1), newest_title(s))


def tool_result(s: Session, tool: str, arguments: dict, query: str = "") -> dict:
    """The CallToolResult of one tools/call, every content block."""
    return s.rpc("tools/call", {"name": tool, "arguments": arguments}, query=query)["result"]


def image_blocks(result: dict) -> list[bytes]:
    """The decoded data of each image block, after checking that the text block comes first."""
    content = result.get("content", [])
    if not content or content[0].get("type") != "text":
        return []
    return [base64.b64decode(b.get("data", "")) for b in content[1:] if b.get("type") == "image"]


def is_png(data: bytes) -> bool:
    return data.startswith(bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]))


def check_images(s: Session) -> None:
    print("== images")
    widget = "/Game/SmokeImages/WBP_SmokeImage"
    s.call("create_widget_blueprint", {"package_path": "/Game/SmokeImages", "name": "WBP_SmokeImage"})
    s.call("add_widget_to_blueprint", {"widget_bp_path": widget, "widget_type": "TextBlock", "widget_name": "Label"})
    s.call("compile_blueprint", {"blueprint_name": widget})
    preview = {"widget_bp_path": widget, "width": 256, "height": 128, "opaque_background": True}
    result = tool_result(s, "render_widget_preview", preview)
    payload = json.loads(result["content"][0]["text"])
    data = payload.get("data", {})
    if not data.get("rendered"):
        s.check("without rendering, render_widget_preview answers with the text alone",
                len(result["content"]) == 1 and not result.get("isError"), result["content"][1:2])
        print("  SKIP the image blocks: this editor cannot render (launch with --render)")
        return
    images = image_blocks(result)
    s.check("render_widget_preview answers with one PNG image block after the text", len(images) == 1 and is_png(images[0])
            and result["content"][1].get("mimeType") == "image/png", result["content"][1:2])
    s.check("its JSON says the image was returned, with its size", data.get("image_returned") is True
            and data.get("image_width") == 256 and data.get("image_height") == 128, data)
    s.check("the reserved key never reaches the client", "_mcp_images" not in result["content"][0]["text"], "")
    off = tool_result(s, "render_widget_preview", {**preview, "return_image": False})
    s.check("return_image=false answers with the text alone", len(off["content"]) == 1
            and json.loads(off["content"][0]["text"]).get("data", {}).get("image_returned") is False, off["content"][1:2])
    off = tool_result(s, "render_widget_preview", preview, query="?images=off")
    s.check("?images=off answers with the text alone", len(off["content"]) == 1, off["content"][1:2])
    through = tool_result(s, "call_tool", {"name": "render_widget_preview", "arguments": preview})
    s.check("call_tool returns the image too", len(image_blocks(through)) == 1, through["content"][1:2])
    item = {"command_type": "render_widget_preview", "params": preview}
    batch = tool_result(s, "execute_batch", {"commands": [item] * 5})
    note = json.loads(batch["content"][0]["text"]).get("image_note", "")
    s.check("execute_batch returns its items' images, at most 4, and names the rest",
            len(image_blocks(batch)) == 4 and "1 image(s)" in note, note)
    big = tool_result(s, "render_widget_preview", {**preview, "width": 3000, "height": 2000})
    big_data = json.loads(big["content"][0]["text"]).get("data", {})
    big_images = image_blocks(big)
    s.check("a picture over the limits comes as a smaller JPEG, with a note", len(big_images) == 1
            and big_images[0].startswith(bytes([0xFF, 0xD8, 0xFF])) and big_data.get("image_width", 9999) <= 1568
            and big_data.get("image_note"), big_data)
    viewport = tool_result(s, "capture_viewport", {})
    if viewport.get("isError") or "No editor viewport" in viewport["content"][0]["text"]:
        print("  SKIP capture_viewport: no editor viewport")
    else:
        s.check("capture_viewport answers with a PNG image block", [is_png(i) for i in image_blocks(viewport)] == [True],
                viewport["content"][0]["text"][:300])


def check_source_control(s: Session) -> None:
    print("== source control, with no provider enabled")
    is_error, status = s.call("get_source_control_file_status", {"asset_paths": ["/Game/Smoke/NoSuchAsset"]})
    data = status.get("data", status)
    s.check("get_source_control_file_status answers enabled=false without an error",
            not is_error and data.get("enabled") is False and data.get("files") == [], status)
    _, checkout = s.call("checkout_files", {"asset_paths": ["/Game/Smoke/NoSuchAsset"]})
    s.check("checkout_files refuses and names the cause", checkout.get("success") is False
            and "disabled" in checkout.get("error", ""), checkout)


POLICY_KEYS = ("bReadOnlyMode", "bAllowCodeExecution", "DisabledDomains", "DisabledTools")


def code_exec_tools(catalog: dict) -> list[str]:
    """The listed tools the catalog marks codeExec (tool_hints.py pins them)."""
    return [c["name"] for d in catalog["domains"] for c in d["commands"] if c.get("codeExec") and not c.get("retired")]


def reset_policy_settings(project: Path) -> None:
    """Drops the tool permissions a previous run saved in the throwaway project's user settings."""
    for ini in (project.parent / "Saved" / "Config").glob("*/EditorPerProjectUserSettings.ini"):
        lines = ini.read_text(encoding="utf-8", errors="replace").splitlines()
        kept = [line for line in lines if line.lstrip("+-.!").split("=", 1)[0].strip() not in POLICY_KEYS]
        if kept != lines:
            ini.write_text("\n".join(kept) + "\n", encoding="utf-8")


def refusal_of(payload: dict) -> dict:
    """The policy refusal a tool answered, or {}."""
    result = payload.get("result")
    return result if isinstance(result, dict) and result.get("refused_by") == "policy" else {}


def console(s: Session, command: str) -> None:
    is_error, reply = s.call("execute_console_command", {"command": command})
    s.check(f"console: {command}", not is_error and reply.get("success") is not False, reply)


def check_policy(s: Session, catalog: dict) -> None:
    print("== tool permissions: a domain and a tool turned off")
    listed = [c for d in catalog["domains"] for c in d["commands"] if not c.get("retired")]
    water = [c["name"] for d in catalog["domains"] if d["domain"] == "Water" for c in d["commands"] if not c.get("retired")]
    headers, _ = s.raw("initialize", {"protocolVersion": "2025-06-18"}, query="?tools=all")
    session = headers.get("mcp-session-id", "")
    s.check("?tools=all opens a session too", bool(session), headers)
    _, light = s.call("spawn_actor", {"type": "PointLight", "name": "SmokePolicy"})
    light_name = light.get("data", light).get("name", "SmokePolicy")
    console(s, "mcp.policy tools=spawn_actor,get_properties domains=Water")

    headers, body = s.raw("ping", headers={"Mcp-Session-Id": session})
    messages = sse_messages(body) if "text/event-stream" in headers.get("content-type", "") else []
    s.check("the session's next request hears notifications/tools/list_changed, then its answer",
            [m.get("method") for m in messages] == ["notifications/tools/list_changed", None] and messages[-1].get("result") == {}, body[:300])
    headers, body = s.raw("ping", headers={"Mcp-Session-Id": session})
    s.check("only once", "application/json" in headers.get("content-type", ""), headers)
    full = [t["name"] for t in s.rpc("tools/list", query="?tools=all")["result"]["tools"]]
    s.check("the full list leaves them out", "spawn_actor" not in full and "get_properties" not in full
            and not set(water) & set(full) and len(full) == len(listed) - len(water) - 2, len(full))
    _, found = s.call("find_tools", {"query": "spawn actor"})
    s.check("find_tools does not find a tool turned off", "spawn_actor" not in [t["name"] for t in found.get("tools", [])], found)
    is_error, found = s.call("find_tools", {"domain": "Water"})
    s.check("find_tools on a domain turned off says so", is_error and "turned off" in found.get("error", ""), found)
    is_error, described = s.call("describe_tool", {"name": "spawn_actor"})
    s.check("describe_tool answers the refusal", is_error and refusal_of(described).get("policy") == "disabled_tool", described)
    is_error, reply = s.call("spawn_actor", {"type": "PointLight", "name": "SmokeRefused"})
    s.check("a direct call is refused (disabled_tool)", is_error and refusal_of(reply).get("policy") == "disabled_tool"
            and "do not retry" in reply.get("error", ""), reply)
    is_error, reply = s.call("call_tool", {"name": "spawn_actor", "arguments": {"type": "PointLight"}})
    s.check("call_tool is refused", is_error and refusal_of(reply).get("policy") == "disabled_tool", reply)
    is_error, reply = s.call(water[0] if water else "get_water_body_info", {})
    s.check("a tool of the domain is refused (disabled_domain)", is_error and refusal_of(reply).get("policy") == "disabled_domain", reply)
    _, batch = s.call("execute_batch", {"commands": [{"command_type": "get_project_info"},
                                                     {"command_type": "spawn_actor", "params": {"type": "PointLight"}}]})
    items = batch.get("results", batch.get("data", {}).get("results", []))
    s.check("an execute_batch item is refused, the others run", [i.get("ok") for i in items] == [True, False]
            and "REFUSED" in (items[1].get("error", "") if len(items) > 1 else ""), batch)
    _, written = s.call("set_properties", {"object_path": light_name, "property_name": "bHidden", "value": True})
    readback = written.get("readback", {})
    s.check("the readback still re-reads with get_properties turned off", written.get("success") is not False
            and readback.get("checked") is True, written)
    console(s, "mcp.policy tools= domains=")
    headers, body = s.raw("ping", headers={"Mcp-Session-Id": session})
    messages = sse_messages(body) if "text/event-stream" in headers.get("content-type", "") else []
    s.check("turning them back on announces it too", [m.get("method") for m in messages][:1] == ["notifications/tools/list_changed"], body[:300])
    full = [t["name"] for t in s.rpc("tools/list", query="?tools=all")["result"]["tools"]]
    s.check("and restores the list", len(full) == len(listed), len(full))

    print("== tool permissions: code execution off")
    console(s, "mcp.policy codeexec=0")
    code_exec = code_exec_tools(catalog)
    full = [t["name"] for t in s.rpc("tools/list", query="?tools=all")["result"]["tools"]]
    s.check(f"the {len(code_exec)} code-execution tools leave the list, nothing else", len(full) == len(listed) - len(code_exec)
            and not set(code_exec) & set(full), sorted(set(code_exec) & set(full)))
    is_error, reply = s.call("execute_python", {"code": "print(1)"})
    s.check("execute_python is refused (code_execution)", is_error and refusal_of(reply).get("policy") == "code_execution", reply)
    is_error, reply = s.call("start_cpp_compile", {})
    s.check("start_cpp_compile is refused (code_execution)", is_error and refusal_of(reply).get("policy") == "code_execution", reply)
    is_error, reply = s.call("spawn_actor", {"type": "PointLight", "name": "SmokeStillWrites"})
    s.check("a write still runs", not is_error and reply.get("success") is not False, reply)


def check_read_only(s: Session, catalog: dict) -> None:
    print("== read-only mode (-McpReadOnly)")
    listed = [c for d in catalog["domains"] for c in d["commands"] if not c.get("retired")]
    allowed = {c["name"] for c in listed if c.get("observes") or c["name"] == "execute_batch"
               or (not c.get("codeExec") and (c.get("annotations") or {}).get("readOnlyHint"))}
    meta = ["find_tools", "describe_tool", "call_tool", "enable_toolset"]
    init = s.rpc("initialize", {"protocolVersion": "2025-06-18"})["result"]
    s.check("initialize says read-only mode is on", "READ-ONLY" in init.get("instructions", ""), init.get("instructions", "")[:200])
    full = [t["name"] for t in s.rpc("tools/list", query="?tools=all")["result"]["tools"]]
    s.check(f"the full list carries the {len(allowed)} reading and observing tools", set(full) == allowed, sorted(set(full) ^ allowed)[:10])
    core = [t["name"] for t in s.rpc("tools/list")["result"]["tools"]]
    s.check("the core list carries the core reads and the meta-tools", set(core) - set(meta) <= allowed and core[-4:] == meta, core)
    compact = [t["name"] for t in s.rpc("tools/list", query="?tools=compact")["result"]["tools"]]
    s.check("the compact list carries the meta-tools", compact == meta[:3], compact)
    cpp = {"get_cpp_compile_status", "start_cpp_compile", "create_cpp_class", "set_live_coding_enabled"}
    s.check("of the C++ tools only get_cpp_compile_status is listed", cpp & set(full) == {"get_cpp_compile_status"},
            sorted(cpp & set(full)))
    tests = {"get_functional_test_status", "pause_play_in_editor", "resume_play_in_editor", "step_play_in_editor_frame",
             "cancel_automation_test"}
    s.check("of the test and PIE control tools only get_functional_test_status is listed",
            tests & set(full) == {"get_functional_test_status"}, sorted(tests & set(full)))
    renamed = next((old for old, new in catalog.get("renamed", {}).items() if new not in allowed), "")
    retired = next((c["name"] for d in catalog["domains"] for c in d["commands"] if c.get("retired")
                    and not (c.get("annotations") or {}).get("readOnlyHint")), "")
    for how, (tool, args) in {
        "directly": ("spawn_actor", {"type": "PointLight"}),
        "through call_tool": ("call_tool", {"name": "spawn_actor", "arguments": {"type": "PointLight"}}),
        f"by a renamed name ({renamed})": (renamed, {}),
        f"by a retired name ({retired})": (retired, {}),
        "as undo": ("undo", {}),
    }.items():
        is_error, reply = s.call(tool, args)
        s.check(f"a write is refused {how}", is_error and refusal_of(reply).get("policy") == "read_only", reply)
    _, batch = s.call("execute_batch", {"commands": [{"command_type": "get_project_info"},
                                                     {"command_type": "spawn_actor", "params": {"type": "PointLight"}}]})
    items = batch.get("results", batch.get("data", {}).get("results", []))
    s.check("as an execute_batch item", [i.get("ok") for i in items] == [True, False]
            and "read-only" in (items[1].get("error", "") if len(items) > 1 else ""), batch)
    is_error, reply = s.call("get_project_info")
    s.check("a read runs", not is_error and reply.get("success") is not False, reply)
    is_error, reply = s.call("capture_viewport", {"width": 64, "height": 64})
    s.check("a capture is not refused", not refusal_of(reply), reply if refusal_of(reply) else "")


def check_tests_and_pie(s: Session) -> None:
    print("== tests and Play-in-Editor control, with nothing running")
    _, paused = s.call("pause_play_in_editor")
    data = paused.get("data", {})
    s.check("pause_play_in_editor without PIE answers pie_active false", paused.get("success") is True
            and data.get("pie_active") is False and data.get("paused") is False, paused)
    _, canceled = s.call("cancel_automation_test")
    data = canceled.get("data", {})
    s.check("cancel_automation_test with no test answers was_running false", canceled.get("success") is True
            and data.get("was_running") is False and data.get("canceled") is False, canceled)
    _, active = s.call("is_play_in_editor_active")
    s.check("is_play_in_editor_active reports is_paused", active.get("data", {}).get("is_paused") is False, active)


def check_prompts(s: Session) -> None:
    print("== prompts and resources")
    skills = sorted(p.name for p in SKILLS.iterdir() if (p / "SKILL.md").is_file())
    init = s.rpc("initialize", {"protocolVersion": "2025-06-18"})["result"]
    s.check("initialize declares prompts and resources",
            {"prompts", "resources"} <= set(init.get("capabilities", {})), init.get("capabilities"))
    s.check("the instructions point to ueagent://guide", "ueagent://guide" in init.get("instructions", ""), "")
    prompts = s.rpc("prompts/list")["result"]["prompts"]
    s.check("one prompt per shipped skill", [p["name"] for p in prompts] == skills, [p["name"] for p in prompts])
    s.check("each prompt has a title, a description and the optional request",
            all(p.get("title") and p.get("description") and p.get("arguments") == [
                {"name": "request", "description": "What you want done, in your own words. Optional.", "required": False}]
                for p in prompts), prompts[:1])
    for name in skills:
        got = s.rpc("prompts/get", {"name": name, "arguments": {"request": "light the level"}}).get("result", {})
        text = (got.get("messages") or [{}])[0].get("content", {}).get("text", "")
        s.check(f"prompts/get {name} returns the skill with the request",
                text.startswith(f"Use the UEAgent skill `{name}` below for this request: light the level\n\n# "), text[:200])
    unknown = s.rpc("prompts/get", {"name": "ueagent-nothing"}).get("error", {})
    s.check("an unknown prompt is Invalid params, with the prompts", unknown.get("code") == -32602
            and skills[0] in unknown.get("message", ""), unknown)
    resources = s.rpc("resources/list")["result"]["resources"]
    s.check("resources/list starts with the guide", resources[:1] and resources[0]["uri"] == "ueagent://guide",
            resources[:1])
    unread = []
    for resource in resources:
        contents = s.rpc("resources/read", {"uri": resource["uri"]}).get("result", {}).get("contents", [])
        if not (len(contents) == 1 and contents[0].get("uri") == resource["uri"] and contents[0].get("text")):
            unread.append(resource["uri"])
    s.check(f"every listed resource reads ({len(resources)})", not unread, unread)
    guide = s.rpc("resources/read", {"uri": "ueagent://guide"})["result"]["contents"][0]["text"]
    s.check("the guide is the tool map, with its tables", "| Domain | Tools | What it covers |" in guide
            and "| Pack | For | Tools |" in guide, guide[:200])
    missing = s.rpc("resources/read", {"uri": "ueagent://skills/../UEAgent.uplugin"}).get("error", {})
    s.check("a URI that is not listed is -32002, never a path", missing.get("code") == -32002
            and missing.get("data") == {"uri": "ueagent://skills/../UEAgent.uplugin"}, missing)
    templates = s.rpc("resources/templates/list").get("result", {})
    s.check("resources/templates/list is empty", templates == {"resourceTemplates": []}, templates)


def launch(args: argparse.Namespace, *extra: str) -> subprocess.Popen:
    """A headless editor on --launch, serving on the port of --url."""
    port = args.url.split("://", 1)[1].split("/", 1)[0].partition(":")[2] or "80"
    binary = args.engine / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
    return subprocess.Popen([str(binary), str(args.launch.resolve()), "-unattended", "-nopause", "-nosplash",
                             "-RenderOffscreen" if args.render else "-NullRHI", "-NoSound", "-log=UEAgentSmoke.log",
                             f"-McpHttpPort={port}", *extra])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://127.0.0.1:55561/mcp", help="The MCP endpoint")
    parser.add_argument("--launch", type=Path, help="A throwaway .uproject: start a headless editor on it")
    parser.add_argument("--engine", type=Path, help="Unreal Engine root (with --launch)")
    parser.add_argument("--render", action="store_true", help="With --launch: render offscreen instead of -NullRHI")
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
        reset_policy_settings(args.launch.resolve())
        editor = launch(args)
        print(f"Started a headless editor (pid {editor.pid}) on {args.launch.name}")
    try:
        if not wait_for_server(session, 600 if editor else 10):
            print(f"FAIL: no UEAgent server answers at {args.url}")
            return 1
        check_protocol(session, catalog)
        check_core_list(session, catalog)
        check_meta_tools(session, catalog)
        check_prompts(session)
        check_cpp_compile(session)
        if editor:
            check_edits(session)
            check_images(session)
            check_source_control(session)
            check_tests_and_pie(session)
            check_policy(session, catalog)
            editor.kill()
            editor.wait(timeout=120)
            reset_policy_settings(args.launch.resolve())
            editor = launch(args, "-McpReadOnly")
            print(f"Started a headless editor in read-only mode (pid {editor.pid})")
            if wait_for_server(session, 600):
                check_read_only(session, catalog)
            else:
                session.check("the read-only editor answers", False, args.url)
        else:
            print("== edit and permission checks skipped: they only run in an editor started with --launch")
    finally:
        if editor:
            editor.kill()
            editor.wait(timeout=120)
            reset_policy_settings(args.launch.resolve())
    print(f"\n{session.passed} passed, {session.failed} failed")
    return 1 if session.failed else 0


if __name__ == "__main__":
    sys.exit(main())
