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

Some clients cap how many tools a server may list, and the full list has more than 1,600. Give them
the [compact tool list](clients.md#compact-tool-list): `?tools=compact` at the end of the URL.

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
