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

## A client connects but times out

The client gives up while it connects (Claude Code: `CONNECT_TIMEOUT`, "Version negotiation probe
timed out"), and the editor's Output Log shows `initialize: <client> ...` again every few seconds,
from a new connection each time. The request reaches the server and the server answers it: the answer
does not reach the client. With curl the response's head arrives, often with
`Transfer-Encoding: chunked`, which UEAgent never sends (each of its responses has a
`Content-Length`), and its body does not.

Something relays the traffic between the client and the editor: a proxy, an antivirus that scans web
traffic, a VPN or a traffic filter. Some finish relaying a response only when the server closes the
connection, and UEAgent keeps a connection open for the client's next request.

- **A proxy in the environment.** Claude Code and curl read the proxy variables (`HTTP_PROXY`,
  `HTTPS_PROXY`, `ALL_PROXY`, and their lower-case forms). Leave the loopback out with
  `NO_PROXY=127.0.0.1,localhost`, in the environment the client starts in, or in the `env` block of
  Claude Code's `settings.json`: `"env": { "NO_PROXY": "127.0.0.1,localhost" }`.
- **An antivirus, VPN or filter.** Leave the loopback, or `UnrealEditor.exe`, out of what it scans,
  or turn it off for a test.
- **Close each connection.** Turn off **HTTP keep-alive** in **Editor Preferences > Plugins >
  UEAgent**, or start the editor with `-McpHttpNoKeepAlive`, and restart the server
  (`mcp.http.restart`). Each response then says `Connection: close`, and the server closes its
  connection after it.

To find which one it is, with the editor open (`curl.exe` in PowerShell):

1. `curl.exe -v --raw http://127.0.0.1:55561/mcp` gets `405 Method Not Allowed` with
   `Content-Length: 0`, and curl ends at once. If it hangs, or the head says
   `Transfer-Encoding: chunked`, something rewrites the response.
2. The same with `--noproxy "*"`: if this one ends at once, a proxy variable is the cause.
3. The same with `-H "Connection: close"`: if this one ends at once, the relay waits for the
   connection to close: turn keep-alive off.
4. A raw socket, which no proxy setting touches:
   `python -c "import socket;s=socket.create_connection(('127.0.0.1',55561));s.sendall(b'GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:55561\r\nConnection: close\r\n\r\n');print(s.recv(4096))"`
   prints the response as the server sent it.
5. `netstat -ano | findstr :55561` lists who listens on the port. A program that listens on
   `[::]:55561` gets the connections of a client whose URL says `localhost` and goes to `::1`: give the
   client `127.0.0.1`.

The server logs each response at Verbose once the network has taken all of it:
`log LogUEAgentHttp Verbose` in the editor console, or `-LogCmds="LogUEAgentHttp Verbose"` on its
command line, shows lines such as `conn 12: POST /mcp initialize -> 200, 1834 body bytes, keep-alive`.
So with that line in the log, the response left the server whole, and what happened to it after that
is outside the editor. A warning `sending failed after N of M bytes` says it did not leave whole.

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
`call_tool`, or adds its pack or its whole domain to the list with `enable_toolset`. If the client does
not show the added tools, it does not re-read its list when the server says it changed: `call_tool`
still runs them, and `?packs=<pack>` or `?domains=<Domain>` in the URL lists them from the start.

## A call is refused while Play-in-Editor runs

Calls that compile a Blueprint or write to disk are refused during PIE: a compile can keep the play
world alive and the engine then stops the editor, and the engine refuses to save while PIE runs.
Stop PIE and retry. `force: true` runs the call anyway, at your own risk.

## A call is refused before it runs

The call is one known to crash or freeze the editor in that situation. The refusal says why, and
what to do instead.

## A call is refused by the tool permissions

The refusal says `refused_by: "policy"`: a setting in **Editor Preferences > Plugins > UEAgent > Permissions**
turns that tool off (read-only mode, code execution off, or a domain or tool turned off). Change the setting, or
use the **Read-only mode** toggle in the status bar's **MCP** menu; `mcp.http.status` prints what is off. When
read-only mode cannot be turned off, the editor was started with `-McpReadOnly` or `UEAGENT_READONLY=1`. The
client re-reads its tool list on its next request that accepts SSE; one that does not, or that has no session,
sees refusals until it lists the tools again (reconnect it).

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
