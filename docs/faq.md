# FAQ

## Which Unreal Engine versions does it support?

Unreal Engine 5.8. The release is built for 5.8, and the code uses 5.8's APIs; other versions are not
tested.

## Which platforms?

Windows (Win64), the only platform the plugin declares.

## Does it work in a Blueprint-only project?

Yes. The release is precompiled, so no compiler is needed, in a Blueprint-only project or a C++ one.
Building from source needs Visual Studio 2022 with the C++ toolchain.

## Does it cost anything?

No. UEAgent is free and open source under the [MIT License](../LICENSE), for personal and commercial
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
[Security](../README.md#security).

## Which clients work?

Any MCP client that speaks Streamable HTTP. [Clients](clients.md) has the setup for Claude Code,
Cursor, VS Code, Windsurf, Claude Desktop, Gemini CLI and Codex.

## My client cannot take 2,000 tools

Connect it with `?tools=compact` at the end of the URL: it then lists three tools that search,
describe and run the rest. See [Compact tool list](../README.md#compact-tool-list).

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
