# Connecting a client

While the editor is open, UEAgent serves MCP over Streamable HTTP at `http://127.0.0.1:55561/mcp`
(the port is set in **Editor Preferences > Plugins > UEAgent**). Any MCP client that speaks
Streamable HTTP can connect.

Every client below except Claude Code loads all the tools up front, so the examples ask for the
[compact tool list](../README.md#compact-tool-list) with `?tools=compact`: three meta-tools that
search, describe and run the rest. Remove it to give a client every tool.

## Claude Code

From the editor's status bar, **MCP · 55561 > Connect Claude Code to this project** writes the
`unreal` entry of the project's `.mcp.json`. Or, for yourself only:

```bash
claude mcp add --transport http unreal http://127.0.0.1:55561/mcp
```

Claude Code loads tools on demand, so it takes the full list.

## Cursor

`.cursor/mcp.json` in the project, or `~/.cursor/mcp.json` for every project:

```json
{
  "mcpServers": {
    "unreal": {
      "url": "http://127.0.0.1:55561/mcp?tools=compact"
    }
  }
}
```

## VS Code (GitHub Copilot, agent mode)

`.vscode/mcp.json` (the root key is `servers`, not `mcpServers`):

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

## Windsurf

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

## Claude Desktop

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

## Gemini CLI

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

## Codex

```bash
codex mcp add unreal --url "http://127.0.0.1:55561/mcp?tools=compact"
```

Or in `~/.codex/config.toml`:

```toml
[mcp_servers.unreal]
url = "http://127.0.0.1:55561/mcp?tools=compact"
```

## With an auth token

When an **Auth token** is set, every request needs the header `Authorization: Bearer <token>`.
Keep the token in an environment variable, such as `UEAGENT_TOKEN`, rather than in a file you
commit:

- Claude Code: the entry that **Connect Claude Code** writes already sends
  `Bearer ${UEAGENT_TOKEN}`.
- Codex: `bearer_token_env_var = "UEAGENT_TOKEN"` in the server's table.
- Claude Desktop: mcp-remote's `--header` option.
- Cursor, VS Code, Windsurf and Gemini CLI: the entry's `headers` field.

## Several clients at once

Several clients, and several agents, can share one editor. Every tool call runs on the editor's game
thread through a single queue, so calls from different clients never run at the same time.
