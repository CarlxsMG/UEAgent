# FAQ

## Which Unreal Engine versions does it support?

Unreal Engine 5.7 and 5.8. Each release has a precompiled zip for each engine, built and tested
separately; other versions are not tested.

A few things need 5.8, because 5.7 has no API for them. On 5.7 they answer an error that says so, and
nothing is changed:

- `apply_adaptive_remesh` (Geometry Script's adaptive remesh is new in 5.8);
- `add_field_node` with `node_type` RotatedRadialVector;
- `add_chooser_column` with `column_type` Name or OutputName;
- `set_modulation_parameter_settings` with `clamp_to_normalized_range`;
- `set_audio_section_properties` with `play_until_finished`.

## Which platforms?

Windows (Win64), the only platform the plugin declares.

## Does it work in a Blueprint-only project?

Yes. The release is precompiled, so no compiler is needed, in a Blueprint-only project or a C++ one.
Building from source needs Visual Studio 2022 with the C++ toolchain.

## Does it cost anything?

No. UEAgent is free and open source under the
[MIT License](https://github.com/CarlxsMG/UEAgent/blob/main/LICENSE), for personal and commercial
projects alike. The MCP client and the model behind it may have costs of their own.

## Does it send my project anywhere?

UEAgent itself does not: no telemetry, no analytics, no update checks. The server only listens on
`127.0.0.1`, and a tool's answer goes to the client you connected. What that client sends to its
model provider is up to the client.

## Is it safe to let an agent drive my editor?

The server takes no connection from other machines or from web pages, and an auth token can lock it
to the clients you configure. Calls known to crash or freeze the editor are refused before they run.

But a connected agent acts with the editor's rights: it can run Python, save, rename and delete
assets, and launch processes. Keep the project under source control, and list vendored content in
**Read Only Content Roots** so that no tool can save, rename, move or delete it. See
[Security](https://github.com/CarlxsMG/UEAgent/blob/main/README.md#security).

## Can I undo what the agent did?

Yes. Every tool call is one undo step, named after the tool ("UEAgent: spawn_actor"), so Ctrl+Z, or
**Edit > Undo History**, reverts it; a `execute_batch` is one step. Undo restores the editor's state: a file
the call saved stays saved until you save again. `mcp.UndoStepPerCall 0` in the editor console
leaves undo to each tool.

## How do I see what the agent did?

Open **Activity** from the MCP menu in the editor's status bar: the last 200 tool calls, newest
first, each with its time, duration, outcome (OK, a readback warning, failed or refused) and detail.
`mcp.activity` in the editor console prints the same list to the log.

## Which clients work?

Any MCP client that speaks Streamable HTTP. [Clients](clients.md) has the setup for Claude Code,
Cursor, VS Code, Windsurf, Claude Desktop, Gemini CLI and Codex.

## My client cannot take 1,600 tools

Connect it with `?tools=compact` at the end of the URL: it then lists three tools that search,
describe and run the rest. See [Compact tool list](clients.md#compact-tool-list).

## Can several agents use the same editor?

Yes. Every tool call runs on the editor's game thread through one queue, so calls from different
clients never run at the same time.

## Does it need Python or Node.js?

No. The server is C++ inside the editor. The plugin enables Unreal's own Python plugin for the tools
that run Python in the editor. Node.js is only needed to connect Claude Desktop, through mcp-remote.

## How do I update it?

Close the editor, replace `Plugins/UEAgent` with the new release, and reconnect your client so it
lists the tools again.

## A tool does something wrong. Where do I report it?

Open an issue with the **Bug report** template: the tool, the call and the lines around it from
`Saved/Logs/<Project>.log`. For a question, use [Discussions](https://github.com/CarlxsMG/UEAgent/discussions/categories/q-a).
