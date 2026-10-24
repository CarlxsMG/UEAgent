# Connecting a client

While the editor is open, UEAgent serves MCP over Streamable HTTP at `http://127.0.0.1:55561/mcp`
(the port is set in **Editor Preferences > Plugins > UEAgent**). Any MCP client that speaks
Streamable HTTP can connect.

By default a client gets the [core tool list](#tool-lists): the core tools, and four tools that search,
describe and run the rest, or add a whole domain to the list. `?tools=all` at the end of the URL lists
every tool, for a client that loads tools on demand.

The editor writes the connection for the clients that read it from a file in the project: in the
status bar, **MCP · 55561 > Connect Claude Code to this project**, or **Connect another client** for
Cursor, VS Code, Antigravity CLI and OpenCode. Only the `unreal` entry is written; the rest of the
file, and its layout, stay as they were. For Codex, the same menu copies its command.

Then install the [skills](skills.md) for your client, from the same menu.

## Your client

<!-- tabs -->
### Claude Code

From the editor's status bar, **MCP · 55561 > Connect Claude Code to this project** writes the
`unreal` entry of the project's `.mcp.json`. Or, for yourself only:

```bash
claude mcp add --transport http unreal http://127.0.0.1:55561/mcp
```

Claude Code loads tools on demand, so it can take every tool: add `?tools=all` to the URL.

### Cursor

**MCP · 55561 > Connect another client > Connect Cursor** writes `.cursor/mcp.json` in the project.
By hand, there or in `~/.cursor/mcp.json` for every project:

```json
{
  "mcpServers": {
    "unreal": {
      "url": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

### VS Code

GitHub Copilot, in agent mode. **MCP · 55561 > Connect another client > Connect VS Code** writes
`.vscode/mcp.json` in the project; by hand (the root key is `servers`, not `mcpServers`):

```json
{
  "servers": {
    "unreal": {
      "type": "http",
      "url": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

### Windsurf

`~/.codeium/windsurf/mcp_config.json` (the key is `serverUrl`), then **Refresh** in the MCP panel:

```json
{
  "mcpServers": {
    "unreal": {
      "serverUrl": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

### Claude Desktop

Its config file only starts local processes, so [mcp-remote](https://www.npmjs.com/package/mcp-remote)
bridges to the server (it needs Node.js). In `claude_desktop_config.json` (**Settings > Developer >
Edit Config**):

```json
{
  "mcpServers": {
    "unreal": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://127.0.0.1:55561/mcp", "--allow-http"]
    }
  }
}
```

Quit Claude Desktop completely and open it again to load the change.

### Antigravity CLI

**MCP · 55561 > Connect another client > Connect Antigravity CLI** writes `.agents/mcp_config.json` in
the project (the key is `serverUrl`); by hand, there or in `~/.gemini/config/mcp_config.json`:

```json
{
  "mcpServers": {
    "unreal": {
      "serverUrl": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

### Gemini CLI

Google replaced Gemini CLI with Antigravity CLI in June 2026; for an install that still runs it:

```bash
gemini mcp add -t http unreal "http://127.0.0.1:55561/mcp"
```

Or in `.gemini/settings.json`, with `httpUrl` (in Gemini CLI, `url` means the older SSE transport):

```json
{
  "mcpServers": {
    "unreal": {
      "httpUrl": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

### Codex

**MCP · 55561 > Connect another client > Copy the "codex mcp add" command**, then paste it into a
terminal: Codex keeps its servers in `~/.codex/config.toml`, for every project.

```bash
codex mcp add unreal --url "http://127.0.0.1:55561/mcp"
```

Or in `~/.codex/config.toml`:

```toml
[mcp_servers.unreal]
url = "http://127.0.0.1:55561/mcp"
```
### OpenCode

**MCP · 55561 > Connect another client > Connect OpenCode** writes `opencode.json` in the project
folder. By hand, there or in `~/.config/opencode/opencode.json` for every project (the key is `mcp`,
and a remote server needs `"type": "remote"`):

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "unreal": {
      "type": "remote",
      "url": "http://127.0.0.1:55561/mcp"
    }
  }
}
```

OpenCode connects to its servers when it starts: open the project in Unreal first, then run `opencode`
in the project folder. `opencode mcp list` should then show `unreal` as connected.
<!-- /tabs -->

## Tool lists

The URL picks which tools a client lists. Whatever the list, every tool runs: `call_tool` reaches the
ones that are not listed.

| URL | Lists | For |
|---|---|---|
| `http://127.0.0.1:55561/mcp` | The core tools, `find_tools`, `describe_tool`, `call_tool` and `enable_toolset` | Every client: the default |
| `http://127.0.0.1:55561/mcp?tools=all` | Every tool | A client that loads tools on demand, such as Claude Code |
| `http://127.0.0.1:55561/mcp?tools=compact` | `find_tools`, `describe_tool` and `call_tool` | A client with a tight cap on how many tools it lists |

### The core list

The 11 core tools are the ones most work starts with: find, read, change, save and undo any asset
or object, edit Blueprint graphs, and Python. With the four tools that reach the rest, the list
holds 15 tools. The [tool reference](tools/README.md) marks them.

| Tool | |
|---|---|
| `find_tools` | Searches every tool by words and/or domain; with no arguments, lists the domains and the packs |
| `describe_tool` | One tool's description and input schema |
| `call_tool` | Runs any tool by name, with the same checks and readback as a direct call |
| `enable_toolset` | Adds a pack, or every tool of a domain, to this client's list |

A pack is the few tools of one job the core leaves out:

| Pack | For |
|---|---|
| `actors` | Find, spawn and delete the actors of the open level |
| `levels` | Open, save and inspect levels, and list what is unsaved |
| `play` | Start, stop and check Play-in-Editor, and capture the viewport |
| `assets` | Delete and duplicate assets, and find what references one |
| `blueprint_authoring` | Create Blueprints, add variables, read their outline, unlink pins, remove nodes and build several wired nodes at once |
| `data_tables` | Read a data table row and change its fields |

The [tool reference](tools/README.md) lists each pack's tools. A domain is every tool of one area,
such as Material or Animation, and can add a hundred tools: prefer a pack when one covers the job.

`enable_toolset` lasts as long as the client's session: the server tells the client that its list
changed, and a client that supports `tools/list_changed` reads it again. To list a pack or a domain
from the start, name it in the URL: `?packs=actors,play` or `?domains=Material,Animation` (either
takes both).

### Every client at once

`UEAGENT_TOOLS=all` or `UEAGENT_TOOLS=compact` in the environment the editor starts in changes the
default for every client; `?tools=core` gives one client the core list back.

## With an auth token

When an **Auth token** is set, every request needs the header `Authorization: Bearer <token>`.
Keep the token in an environment variable, such as `UEAGENT_TOKEN`, rather than in a file you
commit:

- Claude Code: the entry that **Connect Claude Code** writes already sends
  `Bearer ${UEAGENT_TOKEN}`.
- Cursor: the entry that **Connect Cursor** writes sends `Bearer ${env:UEAGENT_TOKEN}`.
- VS Code: the entry that **Connect VS Code** writes asks for the token once (the `ueagent-token`
  input) and keeps it in VS Code's secret storage.
- Codex: the copied command adds `--bearer-token-env-var UEAGENT_TOKEN`
  (`bearer_token_env_var = "UEAGENT_TOKEN"` in the server's table).
- OpenCode: the entry that **Connect OpenCode** writes sends `Bearer {env:UEAGENT_TOKEN}`, and sets
  `"oauth": false` so that OpenCode does not try to sign in with OAuth. If `opencode mcp list` says
  `unreal failed` with a 405, `UEAGENT_TOKEN` is missing or wrong in the shell that started OpenCode.
- Claude Desktop: mcp-remote's `--header` option.
- Windsurf and Gemini CLI: the entry's `headers` field.
- Antigravity CLI documents no headers for a remote server, so **Connect** does not offer it while a
  token is set.

## Several clients at once

Several clients, and several agents, can share one editor. Every tool call runs on the editor's game
thread through a single queue, so calls from different clients never run at the same time.
