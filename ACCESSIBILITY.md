# Accessibility

You meet UEAgent in three places: the MCP client you talk to, the plugin's own UI in the Unreal Editor,
and this documentation. This page says what each one offers today, what it lacks, and how to report a
barrier. Neither the plugin's UI nor the documentation site has been audited against WCAG.

## Working through an agent

Every tool takes text and answers text (JSON over MCP), so what you ask the editor to do, and what it
answers, goes through the client you choose, with that client's screen reader support, voice input and
font settings. That can help where the editor's mouse-driven UI is hard to use. It does not make the
Unreal Editor accessible: UEAgent operates the editor, it does not change the editor's own UI.

## In the editor

| | |
|---|---|
| **Status bar item** (MCP · 55561) | The colour of its dot gives the server's state: green when a client is connected, yellow when the server has no client yet or no tool catalog, red when it did not start, gray when it is off. The tooltip and the top of the menu say the same in words. |
| **Status bar menu** | The editor's standard menu, with a tooltip on every entry. Most entries also have a console command (below). |
| **Activity panel** | The Result column names each outcome (OK, Warning, Failed, Refused) as well as colouring it, and the line above the list counts them. The colours are fixed, chosen for the editor's default dark theme; their contrast on a light theme has not been checked. |
| **Notifications** | A notification closes by itself after 6 seconds (success) or 15 seconds (warning or failure). To read a result at your own pace, run the matching console command: it prints to the Output Log. |
| **Text** | The UI is in English. It is drawn with Slate, so it follows the editor's **Application Scale** (Editor Preferences > General > Appearance). |

The console opens with the key above <kbd>Tab</kbd> (<kbd>`</kbd> on a US keyboard). Its commands do what
the status bar menu does:

| Menu | Console command |
|---|---|
| The state in the tooltip and the menu | `mcp.http.status`, `mcp.claude.status`, `mcp.clients.status` |
| **Activity...** | `mcp.activity [N]` (the last N calls, 20 by default) |
| **Restart the server** | `mcp.http.restart` |
| **Contract self-test** | `mcp.contract.selftest <vectors.json>` |
| **Connect Claude Code to this project** | `mcp.claude.connect` |
| **Connect another client** | `mcp.clients.connect <client>`, with `cursor`, `vscode` or `antigravity` |
| **Skills** | `mcp.skills.status`, `mcp.skills.install`, `mcp.skills.remove` |
| **Copy the "claude mcp add" / "codex mcp add" command** | The commands are written out in [Clients](docs/clients.md) |
| **Settings...** | No command: **Edit > Editor Preferences > Plugins > UEAgent** |

## In the documentation

- The [documentation site](https://carlxsmg.github.io/UEAgent/) is built with Material for MkDocs: it works
  with the keyboard (<kbd>/</kbd> or <kbd>S</kbd> opens the search) and follows the system's light or dark
  setting, with a toggle to change it.
- In the [tool explorer](https://carlxsmg.github.io/UEAgent/explorer/), every control has a visible label,
  the filters are a labelled group, the number of results is announced to screen readers as it changes, and
  each tool opens from a button that says whether it is open.
- Images have a text alternative, or an empty one when they only decorate (the logo). The diagram in the
  README repeats what the list above it says.

## Report a barrier

Open an [issue](https://github.com/CarlxsMG/UEAgent/issues/new/choose) and say it is about accessibility:
it gets the `accessibility` label. Say where it happened (the editor UI, a documentation page, a tool's
answer), what you use (a screen reader, a magnifier, the keyboard only, high contrast) and what stopped you.
