# Connecting a client

While the editor is open, UEAgent serves MCP over Streamable HTTP at `http://127.0.0.1:55561/mcp`
(the port is set in **Editor Preferences > Plugins > UEAgent**). Any MCP client that speaks
Streamable HTTP can connect.

Every client here except Claude Code loads all the tools up front, so the examples ask for the
[compact tool list](#compact-tool-list) with `?tools=compact`. Remove it to give a client every tool.

The editor writes the connection for the clients that read it from a file in the project: in the
status bar, **MCP · 55561 > Connect Claude Code to this project**, or **Connect another client** for
Cursor, VS Code and Antigravity CLI. Only the `unreal` entry is written; the rest of the file, and its
layout, stay as they were. For Codex, the same menu copies its command.

Then install the [skills](skills.md) for your client, from the same menu.

## Your client

<!-- tabs -->
### Claude Code

From the editor's status bar, **MCP · 55561 > Connect Claude Code to this project** writes the
`unreal` entry of the project's `.mcp.json`. Or, for yourself only:

```bash
claude mcp add --transport http unreal http://127.0.0.1:55561/mcp
```

Claude Code loads tools on demand, so it takes the full list.

### Cursor

**MCP · 55561 > Connect another client > Connect Cursor** writes `.cursor/mcp.json` in the project.
By hand, there or in `~/.cursor/mcp.json` for every project:

```json
{
  "mcpServers": {
    "unreal": {
      "url": "http://127.0.0.1:55561/mcp?tools=compact"
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
      "url": "http://127.0.0.1:55561/mcp?tools=compact"
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
      "serverUrl": "http://127.0.0.1:55561/mcp?tools=compact"
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
      "args": ["-y", "mcp-remote", "http://127.0.0.1:55561/mcp?tools=compact", "--allow-http"]
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
      "serverUrl": "http://127.0.0.1:55561/mcp?tools=compact"
    }
  }
}
```

### Gemini CLI

Google replaced Gemini CLI with Antigravity CLI in June 2026; for an install that still runs it:

```bash
gemini mcp add -t http unreal "http://127.0.0.1:55561/mcp?tools=compact"
```

Or in `.gemini/settings.json`, with `httpUrl` (in Gemini CLI, `url` means the older SSE transport):

```json
{
  "mcpServers": {
    "unreal": {
      "httpUrl": "http://127.0.0.1:55561/mcp?tools=compact"
    }
  }
}
```

### Codex

**MCP · 55561 > Connect another client > Copy the "codex mcp add" command**, then paste it into a
terminal: Codex keeps its servers in `~/.codex/config.toml`, for every project.

```bash
codex mcp add unreal --url "http://127.0.0.1:55561/mcp?tools=compact"
```

Or in `~/.codex/config.toml`:

```toml
[mcp_servers.unreal]
url = "http://127.0.0.1:55561/mcp?tools=compact"
```
<!-- /tabs -->

## Compact tool list

For a client that cannot take the whole catalog, the server lists three meta-tools instead:

| Tool | |
|---|---|
| `find_tools` | Searches the tools by words and/or domain; with no arguments, lists the domains |
| `describe_tool` | One tool's description and input schema |
| `call_tool` | Runs a tool by name, with the same checks and readback as a direct call |

Ask for it per client with the URL `http://127.0.0.1:55561/mcp?tools=compact`, or for every client
by starting the editor with the environment variable `UEAGENT_TOOLS=compact` (then `?tools=full`
gives one client the whole list back).

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
- Claude Desktop: mcp-remote's `--header` option.
- Windsurf and Gemini CLI: the entry's `headers` field.
- Antigravity CLI documents no headers for a remote server, so **Connect** does not offer it while a
  token is set.

## Several clients at once

Several clients, and several agents, can share one editor. Every tool call runs on the editor's game
thread through a single queue, so calls from different clients never run at the same time.
