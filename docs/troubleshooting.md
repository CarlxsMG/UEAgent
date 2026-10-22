# Troubleshooting

## A client cannot connect

The server runs inside the editor, so the project has to be open. The editor's Output Log says
whether the server started:

- `UEAgentBridge: MCP HTTP server started on http://127.0.0.1:55561/mcp (...)`: it is up. Check the
  URL in the client, port included.
- `UEAgentBridge: MCP HTTP server NOT started: could not open 127.0.0.1:55561 -- is another editor or
  process using the port?`: another editor, or another program, holds the port. Close it, or pick
  another **HTTP port** in **Editor Preferences > Plugins > UEAgent** (or start the editor with
  `-McpHttpPort=N`), then restart the server from the status-bar menu or with `mcp.http.restart`.

`mcp.http.status` in the editor console prints the server's state at any time.

## 401 Unauthorized

An **Auth token** is set and the request did not carry `Authorization: Bearer <token>`. Give the
client the header (see [Clients](clients.md#with-an-auth-token)), or clear the token.

## 403 Forbidden

The request's `Host` was not the loopback, or it came from a web page (its `Origin`). The server
refuses both, so that no website can drive your editor. Connect to `127.0.0.1` or `localhost`.

## The client warns about too many tools, or slows down

Some clients cap how many tools a server may list. The default list holds the core tools only; if the
client still warns, or other servers fill its cap, give it the
[compact tool list](clients.md#tool-lists): `?tools=compact` at the end of the URL. A URL that ends in
`?tools=all` lists more than 1,500 tools: take it out.

## The agent needs a tool that is not listed

Every tool runs whether it is listed or not. The agent finds it with `find_tools` and runs it with
`call_tool`, or adds its whole domain to the list with `enable_toolset`. If the client does not show
the added tools, it does not re-read its list when the server says it changed: `call_tool` still runs
them, and `?domains=<Domain>` in the URL lists them from the start.

## A call is refused while Play-in-Editor runs

Calls that compile a Blueprint or write to disk are refused during PIE: a compile can keep the play
world alive and the engine then stops the editor, and the engine refuses to save while PIE runs.
Stop PIE and retry. `force: true` runs the call anyway, at your own risk.

## A call is refused before it runs

The call is one known to crash or freeze the editor in that situation. The refusal says why, and
what to do instead.

## "Unknown tool"

The client called a tool this server does not have, often from a tool list it cached from another
version: reconnect it so it lists the tools again. The error names the closest tools.

## The precompiled plugin asks to be rebuilt

Each release has a zip for Unreal Engine 5.7 and one for 5.8, on Windows: use the one whose name matches
your engine. With another engine version, build the plugin from source (Visual Studio 2022 with the C++
toolchain).

## Still stuck

Ask in [Discussions](https://github.com/CarlxsMG/UEAgent/discussions/categories/q-a), or, if it looks
like a bug, open an issue with the **Bug report** template and the lines around the call from
`Saved/Logs/<Project>.log`.
