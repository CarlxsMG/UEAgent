"""
catalog_server.py -- UEAgent's tool list as an MCP server over stdio, without the Unreal Editor.

UEAgent's tools run inside the editor, so a service that inspects MCP servers from a container, as
Glama does for its directory, has nothing to start. This server answers that inspection with the
list the editor serves: initialize, ping and tools/list as the editor answers them, and tools/call
with an error that says where the tools run.

Usage:
  python Tools/catalog_server.py [--tools core|all|compact]
      serves MCP over stdio, one JSON-RPC message per line; tools/list carries the list the editor serves by
      default (the core tools and the meta-tools), or the one --tools names, as the editor's ?tools= does
  python Tools/catalog_server.py --check
      builds the list and talks to this server over stdio, without an editor (the CI runs it)
  python Tools/catalog_server.py --compare [URL]
      compares the three lists with the ones a running editor serves (default http://127.0.0.1:55561/mcp)

The list is built the way FMcpToolCatalog::LoadFromFile builds it (Source/UEAgent/Private/Mcp/
McpToolCatalog.cpp), from Resources/command_catalog.json and the generated PIE-safety and alias
tables. A change to how that C++ builds a schema needs the same change here: --compare, with the
editor open, finds any difference.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "Resources" / "command_catalog.json"
PIE_TABLE = ROOT / "Source" / "UEAgent" / "Private" / "McpPieSafety.gen.cpp"
ALIAS_TABLE = ROOT / "Source" / "UEAgent" / "Private" / "McpParamAliases.gen.cpp"
DESCRIPTOR = ROOT / "UEAgent.uplugin"

EDITOR_URL = "http://127.0.0.1:55561/mcp"

# As in McpHttpServer.cpp: the first one is the latest.
SUPPORTED_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")

INSTRUCTIONS = (
    "This server lists UEAgent's tools without the Unreal Editor. The tools themselves run inside the "
    "editor: install the UEAgent plugin (https://github.com/CarlxsMG/UEAgent) in an Unreal Engine 5.7 or 5.8 "
    f"project, open the project, and connect your MCP client to {EDITOR_URL}."
    "\n\nA tool's name starts with its verb, one verb per operation: get_ reads one thing (get_<x>_info, all of "
    "it), list_ enumerates, find_ searches by name or condition, is_/has_ answer yes or no; create_ makes an asset, "
    "add_ puts a part into an asset or actor, spawn_ places an actor in a world; set_ writes; remove_ takes a part "
    "out, delete_ deletes an asset or actor, clear_ empties a list; diff_ compares two things; execute_ runs the "
    "Python, console command or batch you pass, run_ starts a test, query, tree or utility that already exists."
    "\n\nSimilar tools: get_properties reads named properties of one "
    "object you can path to, get_object_info one you know nothing about, get_actor_properties_by_class "
    "placed actors by class or label, get_component_properties a whole component. set_properties writes "
    "any setting of an asset, an actor or a component (\"Actor.Component\", by name or class) -- a light, fog, "
    "sky, decal, camera, post-process volume, sound, Niagara system... -- and saves an asset with save=true; "
    "set_component_property writes a component in a live PIE world. spawn_actor places an actor of any class "
    "with its properties, spawn_box_volume a volume. A typed tool says in its description what it does that "
    "these do not."
)
CALL_ERROR = (
    "UEAgent's tools run inside the Unreal Editor, and this server only lists them. Install the UEAgent "
    "plugin (https://github.com/CarlxsMG/UEAgent) in an Unreal Engine 5.7 or 5.8 project, open the project, and "
    f"connect your MCP client to {EDITOR_URL}."
)

# The texts below are McpToolCatalog.cpp's, word for word.
FORCE_DESCRIPTION = (
    "Optional. This command is REFUSED while a Play-in-Editor session is running: it either compiles a Blueprint "
    "(the reinstancer can retain a world, and CheckForWorldGCLeaks is Fatal -- it KILLS the editor) or writes to disk "
    "(Editor.AllowSavingAssetsDuringPIE=0 makes the engine refuse the write, so it would report success without writing "
    "a byte). Stop PIE and retry instead; force=true runs it anyway, at your own risk."
)
ROTATION_PREFIX = (
    'PREFERRED: the OBJECT {"roll":R,"pitch":P,"yaw":Y} (any subset, absent axes are 0). The ARRAY form below '
    "is [pitch, yaw, roll] -- its index 2 is ROLL, NOT yaw, which once put a PlayerStart on its side. "
)
ALIAS_DESCRIPTION = (
    "Alias of `{0}` (same value, same meaning). Accepted because sibling tools spell this parameter "
    "differently; prefer `{0}`."
)

ENUM_TRIGGER = re.compile(r"\bone of\b|\beither\b|\(default\)|\bor\b", re.IGNORECASE)
QUOTED = re.compile(r"'([^']*)'")
ENUM_TOKEN = re.compile(r"^[a-z][a-z0-9_]{1,39}$")
UNQUOTED_ALTERNATIVES = re.compile(r"\b(?:to|one of|either)\s+((?:[a-z]+\s*,\s*)+(?:or\s+)?[a-z]+)", re.IGNORECASE)
ALTERNATIVE_SEPARATOR = re.compile(r",|\bor\b")
PIE_ROW = re.compile(r'\{\s*TEXT\("([^"]+)"\),\s*EReason::')
ALIAS_TOKEN = re.compile(r'TEXT\("([^"]*)"\)|(nullptr)')


def enum_values(description: str) -> list[str]:
    """The closed set of values a description names, or [] unless it is provably an enum."""
    if not description or not ENUM_TRIGGER.search(description):
        return []
    quoted = QUOTED.findall(description)
    if not quoted or not all(ENUM_TOKEN.match(value) for value in quoted):
        return []
    values = list(dict.fromkeys(quoted))
    if not 2 <= len(values) <= 12:
        return []
    # Alternatives listed without quotes: then the quoted values are only some of the valid ones.
    alternatives = UNQUOTED_ALTERNATIVES.search(QUOTED.sub("<Q>", description))
    if alternatives:
        known = {value.lower() for value in values}
        for word in ALTERNATIVE_SEPARATOR.split(alternatives.group(1)):
            word = word.strip()
            if word and word.lower() not in known:
                return []
    return values


def type_schema(json_type: str) -> dict:
    json_type = json_type.lower()  # FString compares without case
    if json_type in ("string", "number", "integer", "boolean", "array"):
        return {"type": json_type}
    if json_type == "object":
        return {"type": "object", "additionalProperties": True}
    branches: list[dict] = [{"type": name} for name in ("string", "number", "integer", "boolean", "array")]
    branches.append({"type": "object", "additionalProperties": True})
    return {"anyOf": branches}


def rotation_schema() -> dict:
    axes = {axis: {"type": "number"} for axis in ("roll", "pitch", "yaw")}
    return {"anyOf": [{"type": "object", "properties": axes, "additionalProperties": False}, {"type": "array"}]}


def looks_like_rotation(name: str) -> bool:
    low = name.lower()
    return low == "rot" or low.endswith("rotation") or low.endswith("rotator")


def pie_unsafe_commands(table: str) -> set[str]:
    return {name.lower() for name in PIE_ROW.findall(table)}


def alias_rules(table: str) -> dict[str, list[tuple[str, str]]]:
    """`command, alias, canonical, ..., nullptr` per command, then a final nullptr."""
    start = table.index("GRows[] =")
    tokens = iter(ALIAS_TOKEN.findall(table[start:table.index("};", start)]))
    rules: dict[str, list[tuple[str, str]]] = {}
    for command, end in tokens:
        if end:
            break
        pairs = rules.setdefault(command.lower(), [])
        for alias, end in tokens:
            if end:
                break
            canonical, _ = next(tokens)
            pairs.append((alias, canonical))
    return rules


def published_description(command: dict) -> str:
    """The description, then "Returns: " and the `@returns` shape (FTool::PublishedDescription)."""
    description = command.get("description") or ""
    returns = (command.get("returns") or "").strip()
    return f"{description}\n\nReturns: {returns}" if returns else description


def build_tools(catalog: dict, pie_unsafe: set[str], aliases: dict[str, list[tuple[str, str]]]) -> list[dict]:
    tools: list[dict] = []
    seen: set[str] = set()
    for domain in catalog.get("domains", []):
        for command in domain.get("commands", []) if isinstance(domain, dict) else []:
            if not isinstance(command, dict):
                continue
            name = command.get("name") or ""
            if not name or name.lower() in seen:
                continue  # no name, or a duplicate: the first one wins
            if command.get("retired"):
                continue  # still runs in the editor, but tools/list leaves it out

            params: list[dict] = []
            for raw in command.get("params") or []:
                if not isinstance(raw, dict):
                    continue
                param_name = raw.get("name") or ""
                if not param_name or any(p["name"].lower() == param_name.lower() for p in params):
                    continue
                json_type = raw.get("jsonType") or "any"
                description = raw.get("description") or ""
                params.append({
                    "name": param_name,
                    "required": raw.get("required") is True,
                    "description": description,
                    "type": json_type,
                    "enum": enum_values(description) if json_type.lower() == "string" else [],
                    "rotation": json_type.lower() == "array" and looks_like_rotation(param_name),
                    "alias_of": raw.get("aliasOf") or "",
                })

            def find(param_name: str, params: list[dict] = params) -> dict | None:
                return next((p for p in params if p["name"].lower() == param_name.lower()), None)

            properties: dict[str, dict] = {}
            for param in params:
                schema = rotation_schema() if param["rotation"] else type_schema(param["type"])
                description = param["description"]
                lowered = description.lower()
                if param["rotation"] and '"roll"' not in lowered and '"pitch"' not in lowered:
                    description = ROTATION_PREFIX + description
                if description:
                    schema["description"] = description
                if param["enum"]:
                    schema["enum"] = list(param["enum"])
                properties[param["name"]] = schema

            if name.lower() in pie_unsafe and not find("force"):
                properties["force"] = {"type": "boolean", "description": FORCE_DESCRIPTION}

            # The handler's own spellings, marked `aliasOf` the one the catalog requires: any of
            # them satisfies it.
            relaxed: set[str] = set()
            for param in params:
                canonical = find(param["alias_of"]) if param["alias_of"] else None
                if canonical and canonical is not param and canonical["required"]:
                    relaxed.add(canonical["name"].lower())
            for alias, canonical_name in aliases.get(name.lower(), []):
                canonical = find(canonical_name)
                if not canonical or find(alias) or any(key.lower() == alias.lower() for key in properties):
                    continue
                schema = json.loads(json.dumps(properties[canonical["name"]]))
                schema["description"] = ALIAS_DESCRIPTION.format(canonical["name"])
                properties[alias] = schema
                if canonical["required"]:
                    relaxed.add(canonical["name"].lower())

            input_schema: dict = {"type": "object", "properties": properties}
            required = [p["name"] for p in params if p["required"] and p["name"].lower() not in relaxed]
            if required:
                input_schema["required"] = required
            tool = {"name": name, "description": published_description(command), "inputSchema": input_schema}
            if isinstance(command.get("annotations"), dict):
                tool["annotations"] = command["annotations"]
            tools.append(tool)
            seen.add(name.lower())
    return tools


# The meta-tools, as McpMetaTools::BuildMetaTools builds them (Source/UEAgent/Private/Mcp/McpMetaTools.cpp), word for word.
META_DEFAULT_LIMIT = 25
META_MAX_LIMIT = 300


def meta_tools(tools: list[dict], domains: list[str], core_count: int, core: bool) -> list[dict]:
    """find_tools, describe_tool and call_tool, then enable_toolset when the list is the core one."""
    def prop(json_type: str, description: str) -> dict:
        return {"type": json_type, "description": description}

    def tool(name: str, description: str, properties: dict, required: list[str], read_only: bool) -> dict:
        schema: dict = {"type": "object", "properties": properties}
        if required:
            schema["required"] = required
        out = {"name": name, "description": description, "inputSchema": schema}
        if read_only:
            out["annotations"] = {"readOnlyHint": True}
        return out

    domain = prop("string", "Only this domain's tools; without a query, lists them all.")
    domain["enum"] = list(domains)
    find_properties = {
        "query": prop("string", 'Words from the tool\'s name or purpose, e.g. "spawn actor" or "material parameter". '
                                "A plural also finds the singular."),
        "domain": domain,
        "limit": prop("integer", f"How many tools to return (default {META_DEFAULT_LIMIT}, at most {META_MAX_LIMIT})."),
    }
    arguments = prop("object", "The tool's own parameters, as describe_tool lists them.")
    arguments["additionalProperties"] = True
    out = [
        tool("find_tools",
             f"Searches this server's {len(tools)} Unreal Editor tools ({len(domains)} domains) and returns the best matches: "
             "name, domain, parameters (* = required) and a one-line summary. With no arguments it lists the domains. Then "
             "describe_tool gives a tool's full schema and call_tool runs it"
             + (", or enable_toolset adds its whole domain to your tool list" if core else "") + ".",
             find_properties, [], True),
        tool("describe_tool",
             "One tool's full description and input JSON schema: every parameter's type and meaning, the required ones, closed "
             "value sets and accepted aliases. Read it before call_tool unless the parameter list from find_tools is enough.",
             {"name": prop("string", "The tool's exact name, as find_tools returns it.")}, ["name"], True),
        tool("call_tool",
             "Runs one of this server's tools by name, with `arguments` holding that tool's own parameters. The call goes through "
             "the same validation, guards and readback as a tool listed directly.",
             {"name": prop("string", "The tool to run, as find_tools returns it."), "arguments": arguments}, ["name"], False),
    ]
    if core:
        domains_property = prop("array", 'The domains whose tools to add, as find_tools lists them, e.g. ["Material"] or '
                                         '["Animation", "Audio"].')
        domains_property["items"] = {"type": "string", "enum": list(domains)}
        out.append(tool(
            "enable_toolset",
            "Adds every tool of one or more domains to this connection's tool list, so you call them directly instead of "
            "through call_tool: use it when a task needs several tools of a domain, for example Material or Animation. "
            f"The {core_count} core tools are always listed; find_tools with no arguments lists the {len(domains)} domains and "
            "their sizes. Your client re-reads the list when the server announces the change; if it does not, call_tool "
            "reaches the same tools. Lasts until the connection's session ends.",
            {"domains": domains_property}, ["domains"], True))
    return out


def load_lists() -> dict[str, list[dict]]:
    """The three lists the editor serves (FMcpToolCatalog): `all` the catalog's listed tools, `core` the @core tools then
    the meta-tools and enable_toolset (the default), `compact` the three meta-tools alone."""
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    pie_unsafe = pie_unsafe_commands(PIE_TABLE.read_text(encoding="utf-8"))
    aliases = alias_rules(ALIAS_TABLE.read_text(encoding="utf-8"))
    tools = build_tools(catalog, pie_unsafe, aliases)
    core_names = {command["name"] for domain in catalog.get("domains", []) for command in domain.get("commands", [])
                  if command.get("core") is True and not command.get("retired")}
    domain_of = {command["name"]: domain["domain"] for domain in catalog.get("domains", [])
                 for command in domain.get("commands", [])}
    domains = list(dict.fromkeys(domain_of[tool["name"]] for tool in tools))
    core = [tool for tool in tools if tool["name"] in core_names]
    return {
        "all": tools,
        "core": core + meta_tools(tools, domains, len(core), core=True),
        "compact": meta_tools(tools, domains, len(core), core=False),
    }


def load_tools() -> list[dict]:
    return load_lists()["all"]


def server_version() -> str:
    try:
        return json.loads(DESCRIPTOR.read_text(encoding="utf-8-sig")).get("VersionName") or "unknown"
    except (OSError, ValueError):
        return "unknown"


def answer(message: object, tools: list[dict], version: str) -> dict | None:
    """The reply to one JSON-RPC message, or None when it needs none (a notification)."""
    if not isinstance(message, dict):
        return {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid Request"}}
    if "id" not in message or "method" not in message:
        return None
    message_id = message["id"]
    method = message["method"]
    params = message.get("params") if isinstance(message.get("params"), dict) else {}

    def result(value: dict) -> dict:
        return {"jsonrpc": "2.0", "id": message_id, "result": value}

    def error(code: int, text: str) -> dict:
        return {"jsonrpc": "2.0", "id": message_id, "error": {"code": code, "message": text}}

    if method == "tools/call":
        if not isinstance(params.get("name"), str) or not params["name"]:
            return error(-32602, "Invalid params: tools/call needs params.name")
        return result({"content": [{"type": "text", "text": CALL_ERROR}], "isError": True})
    if method == "tools/list":
        return result({"tools": tools})
    if method == "initialize":
        requested = params.get("protocolVersion")
        listed = [tool["name"] for tool in tools]
        instructions = INSTRUCTIONS
        if "enable_toolset" in listed:
            instructions = (f"The editor lists {len(listed) - 4} core tools by default and reaches the rest with find_tools, "
                            "describe_tool, call_tool and enable_toolset; ?tools=all at the end of its URL lists every tool.\n\n"
                            + instructions)
        return result({
            "protocolVersion": requested if requested in SUPPORTED_VERSIONS else SUPPORTED_VERSIONS[0],
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "UEAgent", "version": version},
            "instructions": instructions,
        })
    if method == "ping":
        return result({})
    return error(-32601, f"Method not found: {method}")


def serve(mode: str) -> int:
    tools, version = load_lists()[mode], server_version()
    for raw in sys.stdin.buffer:
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            reply: object = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
        else:
            if isinstance(message, list):
                replies = [r for r in (answer(m, tools, version) for m in message) if r is not None]
                reply = replies or None
            else:
                reply = answer(message, tools, version)
        if reply is not None:
            sys.stdout.buffer.write(json.dumps(reply, separators=(",", ":")).encode("utf-8") + b"\n")
            sys.stdout.buffer.flush()
    return 0


def check() -> int:
    """Builds the list, then runs this script as a server and talks to it over stdio."""
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    commands = sum(not command.get("retired") for domain in catalog["domains"] for command in domain["commands"])
    tools = load_tools()
    with_force = sum("force" in tool["inputSchema"]["properties"] for tool in tools)
    aliases = sum(
        schema.get("description", "").startswith("Alias of `")
        for tool in tools for schema in tool["inputSchema"]["properties"].values())
    problems = []
    if len(tools) != commands:
        problems.append(f"{len(tools)} tools for {commands} commands in the catalog")
    if not with_force:
        problems.append("no tool has `force`: the PIE-safety table was not read")
    if not aliases:
        problems.append("no alias parameter: the alias table was not read")
    # A name another spelling stands for is never in `required`: the server accepts either.
    by_name = {tool["name"]: tool for tool in tools}
    spellings = [(command["name"], param["aliasOf"]) for domain in catalog["domains"]
                 for command in domain["commands"] if not command.get("retired")
                 for param in command.get("params") or [] if param.get("aliasOf")]
    still_required = sorted({f"{tool}.{name}" for tool, name in spellings
                             if name in by_name[tool]["inputSchema"].get("required", [])})
    if not spellings:
        problems.append("no parameter is `aliasOf` another: the handlers' alias chains are not in the catalog")
    if still_required:
        problems.append(f"required although another spelling may replace it: {still_required[:5]}")
    without_annotations = [tool["name"] for tool in tools if "annotations" not in tool]
    if without_annotations:
        problems.append(f"{len(without_annotations)} tools without annotations: {without_annotations[:5]}")
    read_only = sum(tool.get("annotations", {}).get("readOnlyHint") is True for tool in tools)
    with_returns = sum("\n\nReturns: " in tool["description"] for tool in tools)
    if not with_returns:
        problems.append("no description carries its `Returns:` shape: the catalog's `returns` was not read")
    lists = load_lists()
    core, compact = lists["core"], lists["compact"]
    meta = ["find_tools", "describe_tool", "call_tool", "enable_toolset"]
    core_tools = [tool for tool in core if tool["name"] not in meta]
    if [tool["name"] for tool in core[-4:]] != meta:
        problems.append(f"the core list does not end with the four meta-tools: {[t['name'] for t in core[-4:]]}")
    if not 10 <= len(core_tools) <= 36:
        problems.append(f"{len(core_tools)} core tools: the catalog's @core annotations were not read, or are too many")
    if any(tool not in tools for tool in core_tools):
        problems.append("a core tool differs from its entry in the full list")
    if [tool["name"] for tool in compact] != meta[:3]:
        problems.append(f"the compact list is not the three meta-tools: {[t['name'] for t in compact]}")

    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": tools[0]["name"], "arguments": {}}},
        {"jsonrpc": "2.0", "id": 4, "method": "resources/list"},
    ]
    stdin = "".join(json.dumps(r) + "\n" for r in requests).encode("utf-8")
    done = subprocess.run([sys.executable, str(Path(__file__).resolve())], input=stdin, capture_output=True, timeout=120)
    done_all = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--tools", "all"], input=stdin,
                              capture_output=True, timeout=120)
    replies_all = {reply.get("id"): reply for reply in map(json.loads, done_all.stdout.decode("utf-8").splitlines())}
    replies = {reply.get("id"): reply for reply in map(json.loads, done.stdout.decode("utf-8").splitlines())}
    expected = {
        "initialize negotiates 2025-06-18":
            replies.get(1, {}).get("result", {}).get("protocolVersion") == "2025-06-18",
        "the notification gets no reply": len(replies) == 4,
        "tools/list over stdio lists the core list by default": replies.get(2, {}).get("result", {}).get("tools") == core,
        "tools/list with --tools all lists every tool": replies_all.get(2, {}).get("result", {}).get("tools") == tools,
        "tools/call answers isError": replies.get(3, {}).get("result", {}).get("isError") is True,
        "an unknown method is -32601": replies.get(4, {}).get("error", {}).get("code") == -32601,
    }
    problems += [name for name, ok in expected.items() if not ok]
    print(f"{len(tools)} tools, {with_force} with `force`, {aliases} alias parameters, "
          f"{len(spellings)} handler spellings, {read_only} read-only, {with_returns} with `Returns:`, served over stdio; "
          f"{len(core_tools)} core tools listed by default")
    for problem in problems:
        print(f"  FAIL {problem}")
    return 1 if problems else 0


def compare(url: str) -> int:
    """Compares the three lists with the ones the editor at `url` serves."""
    lists = load_lists()
    failed = 0
    for mode in ("all", "core", "compact"):
        print(f"--- ?tools={mode}")
        failed |= compare_list(f"{url}?tools={mode}", lists[mode])
    return failed


def compare_list(url: str, ours: list[dict]) -> int:
    """Compares one list with the one the editor at `url` serves."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}).encode("utf-8")
    request = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    with urllib.request.urlopen(request, timeout=300) as response:
        editor = json.loads(response.read().decode("utf-8"))["result"]["tools"]
    by_name = {tool["name"]: tool for tool in editor}
    missing = [tool["name"] for tool in editor if tool["name"] not in {t["name"] for t in ours}]
    extra = [tool["name"] for tool in ours if tool["name"] not in by_name]
    different = [tool["name"] for tool in ours if tool["name"] in by_name and tool != by_name[tool["name"]]]
    same_order = [tool["name"] for tool in ours] == [tool["name"] for tool in editor]
    print(f"editor: {len(editor)} tools; here: {len(ours)}; missing here: {len(missing)}, extra here: {len(extra)}, "
          f"different: {len(different)}, same order: {same_order}")
    for name in (missing + extra)[:10]:
        print(f"  ONLY ON ONE SIDE {name}")
    for name in different[:10]:
        theirs = by_name[name]
        mine = next(tool for tool in ours if tool["name"] == name)
        print(f"  DIFFERENT {name}\n    editor: {json.dumps(theirs)[:600]}\n    here:   {json.dumps(mine)[:600]}")
    return 0 if not (missing or extra or different) and same_order else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="check the list and the stdio protocol, then exit")
    group.add_argument("--compare", nargs="?", const=EDITOR_URL, metavar="URL",
                       help="compare the three lists with a running editor's")
    parser.add_argument("--tools", choices=("core", "all", "full", "compact"), default="core",
                        help="the list tools/list carries (default core, as the editor's)")
    args = parser.parse_args()
    if args.check:
        return check()
    if args.compare:
        return compare(args.compare)
    return serve("all" if args.tools == "full" else args.tools)


if __name__ == "__main__":
    sys.exit(main())
