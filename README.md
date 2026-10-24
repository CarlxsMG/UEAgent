<p align="center">
  <img src="docs/assets/logo.svg" alt="UEAgent logo" width="120">
</p>

<h1 align="center">UEAgent</h1>

<p align="center">
  <b>The MCP server for Unreal Engine 5, built into the editor.</b><br>
  More than 1,500 tools that let Claude Code, Cursor or any MCP client build in Unreal,<br>
  with every change checked before it runs and read back after.
</p>

<p align="center">
  <img alt="Unreal Engine 5.7 | 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.7%20%7C%205.8-0E1128?logo=unrealengine&logoColor=white">
  <img alt="MCP: Streamable HTTP" src="https://img.shields.io/badge/MCP-Streamable%20HTTP-6E56CF">
  <img alt="1,500+ tools" src="https://img.shields.io/badge/tools-1%2C500%2B-2EA44F">
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue"></a>
  <a href="https://github.com/CarlxsMG/UEAgent/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/CarlxsMG/UEAgent/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://carlxsmg.github.io/UEAgent/"><img alt="Docs" src="https://img.shields.io/badge/docs-carlxsmg.github.io%2FUEAgent-6E56CF"></a>
  <a href="https://glama.ai/mcp/servers/CarlxsMG/UEAgent"><img alt="UEAgent MCP server on Glama" src="https://glama.ai/mcp/servers/CarlxsMG/UEAgent/badges/score.svg"></a>
</p>

<p align="center"><b>English</b> · <a href="README.es.md">Español</a></p>

---

UEAgent is an Unreal Engine 5.7 and 5.8 editor plugin that lets AI agents operate the editor through the
[Model Context Protocol](https://modelcontextprotocol.io). The plugin **is** the server: it serves
Streamable HTTP from inside the editor, with no Python or Node process in between.

## Why UEAgent

- **The whole engine, not a demo.** Blueprints and their graphs, materials, animation (AnimBP,
  montages, IK, Pose Search), Niagara, landscapes and foliage, World Partition, PCG, Sequencer,
  UMG/CommonUI/MVVM, GAS, AI (Behavior Trees, StateTree, EQS, Smart Objects), audio and MetaSounds,
  USD and Interchange, PIE control and inspection: see the [tool reference](docs/tools/README.md).
- **Verified, not assumed.** An editor call that answers `success: true` proves nothing, so every
  call goes through a contract:
  - *preconditions* refuse the calls known to crash or freeze the editor, and say what to do instead;
  - *readback* re-reads the state after the call, and reports as failed a write that did not stick,
    a zero that proves nothing or a change that landed in the wrong PIE world.
- **Safe with your content.** Every tool call is one undo step, named after the tool, so Ctrl+Z
  reverts it. Vendored libraries listed as **Read Only Content Roots** cannot be saved, renamed,
  moved or deleted by any tool, and calls that are unreliable while Play-in-Editor runs are refused
  while it runs.
- **Built for real sessions.** Several agents can share one editor: protocol work runs on network
  threads, and every tool call runs on the game thread through one queue. The **Activity** panel
  (status bar > MCP > Activity) lists each call with its duration and outcome.

```mermaid
flowchart LR
    A["AI agent<br/>Claude Code, Cursor, ..."] -- "MCP over HTTP" --> P
    subgraph E["Unreal Editor + UEAgent"]
        P["Preconditions<br/>refuse known crashes"] --> H["Tool<br/>1,500+ of them"] --> R["Readback<br/>re-read the state"]
    end
    R -- "verified result" --> A
```

How a server inside the editor differs from one outside it, and the questions to ask any MCP server
for Unreal: [Choosing an MCP server](docs/choosing.md).

## Quick start

**Let your agent do it.** Open Claude Code, Cursor, Codex or another agent in your project's folder and
paste:

```
Install the UEAgent plugin in this Unreal Engine project and connect yourself to it, following https://raw.githubusercontent.com/CarlxsMG/UEAgent/main/llms-install.md
```

It does the steps below, checks the download's SHA-256 and asks before it changes anything; you reload
its MCP servers at the end. See [llms-install.md](llms-install.md). Or by hand:

1. **Install.** Download the latest `UEAgent-*-UE5.8-Win64.zip` (or `-UE5.7-`, for your engine) from
   [Releases](https://github.com/CarlxsMG/UEAgent/releases) and unzip it into
   `<YourProject>/Plugins/`, or [build it from source](#build-from-source).
2. **Open the project.** The status bar shows **MCP · 55561** with a green dot once the server is up
   (if it does not, see [Troubleshooting](docs/troubleshooting.md)).
3. **Connect an agent.** From that status-bar menu, **Connect Claude Code to this project** writes the
   `unreal` entry of your project's `.mcp.json`, and **Connect another client** does the same for
   Cursor, VS Code, Antigravity CLI and OpenCode (and copies the command for Codex). Or, for yourself
   only:

   ```
   claude mcp add --transport http unreal http://127.0.0.1:55561/mcp
   ```

   Any MCP client that speaks Streamable HTTP can connect to the same URL: see [Clients](#clients).
4. **Ask.** For example: *"Create a Blueprint actor BP_Door with a door mesh and a box trigger, and
   open the door with a timeline when the player overlaps the trigger."*

## What you can ask

| Ask | Tools it uses |
|---|---|
| "Create BP_Door: a mesh and a box trigger that open the door with a timeline on overlap." | `create_blueprint`, `add_component_to_blueprint`, `build_blueprint_graph`, `compile_blueprint` |
| "Find every Blueprint under /Game that does not compile and tell me why." | `find_erroring_blueprints`, `get_graph_nodes`, `lint_blueprint_graph` |
| "Make a red variant of M_Base and put it on the crates in this level." | `create_material_instance`, `get_actors_by_class`, `set_actor_material` |
| "Play the level, read the player's health, take a screenshot, stop." | `start_play_in_editor`, `get_properties`, `capture_pie_screenshot`, `stop_play_in_editor` |
| "Cut a shot: camera cut, a fade in, and the intro sound at frame 0." | `create_level_sequence`, `add_camera_cut_track`, `add_fade_track_to_sequence`, `add_audio_section` |
| "Which of my assets still reference the vendor pack, and which classes inherit from it?" | `audit_asset_references`, `audit_class_purity`, `run_map_check` |

More in [Examples](docs/examples.md).

## Clients

| Client | Notes |
|---|---|
| Claude Code | One-click connection from the editor's status bar. It loads tools on demand, so `?tools=all` at the end of the URL gives it every tool. |
| Cursor, VS Code, Windsurf, Claude Desktop, Gemini CLI, Codex | Any client with Streamable HTTP: see [the setup for each one](docs/clients.md). |

By default the server lists its 11 core tools, and four tools that search, describe and run the rest or
add a pack or a whole domain to the list. `?tools=all` at the end of the URL lists every tool, and
`?tools=compact` only three: see [tool lists](docs/clients.md#tool-lists).

## Directories

UEAgent is listed in:

- the [official MCP Registry](https://registry.modelcontextprotocol.io), as `io.github.CarlxsMG/UEAgent`,
  which other directories and clients read from;
- [Glama](https://glama.ai/mcp/servers/CarlxsMG/UEAgent), the MCP server directory;
- [awesome-mcp-servers](https://github.com/punkpeye/awesome-mcp-servers#gaming), under Gaming;
- [awesome-unreal](https://github.com/insthync/awesome-unreal#editor), under Editor.

<a href="https://glama.ai/mcp/servers/CarlxsMG/UEAgent"><img width="380" height="200" src="https://glama.ai/mcp/servers/CarlxsMG/UEAgent/badges/card.svg" alt="UEAgent MCP server on Glama"></a>

## Skills

UEAgent ships agent skills in the open Agent Skills format, for Claude Code, Codex, Cursor, VS Code,
Antigravity CLI, Gemini CLI and OpenCode. Install them into the project from the editor's status bar:
**MCP · 55561 > Skills**. They come with the plugin, so they always match its tools.

| Skill | |
|---|---|
| `ueagent-skill-builder` | Turns a workflow you want to repeat into a new skill: it finds the tools, rehearses the task in the editor, checks the result and writes the `SKILL.md` |
| `ueagent-blueprint-actor` | A Blueprint actor from a description: components, graph, a clean compile, and an instance in the level |
| `ueagent-project-audit` | What is broken and what can go, most serious first |
| `ueagent-level-lighting` | Outdoor lighting (sun, sky, sky light, fog) and a screenshot |
| `ueagent-report-issue` | Reports a bug or asks for a tool as a GitHub issue, with the versions, the call and the log |
| `ueagent-contribute-fix` | Turns a change to the plugin's code into a pull request, following the repository's rules |

The client loads them by itself when a request matches. More in [Skills](docs/skills.md).

## Documentation

Also on the web, with search and a tool explorer: [carlxsmg.github.io/UEAgent](https://carlxsmg.github.io/UEAgent/).

| | |
|---|---|
| [Clients](docs/clients.md) | Connecting Claude Code, Cursor, VS Code, Windsurf, Claude Desktop, Gemini CLI and Codex |
| [Examples](docs/examples.md) | Prompts that work well, and the tools they use |
| [Skills](docs/skills.md) | The Claude Code plugin: a skill builder and three skills made with it |
| [Tool reference](docs/tools/README.md) | Every tool, by domain |
| [Answers and refusals](docs/responses.md) | What a call returns, and how to read a refusal or a readback verdict |
| [FAQ](docs/faq.md) | Versions, safety, cost |
| [Troubleshooting](docs/troubleshooting.md) | When a client cannot connect or a call is refused |
| [Architecture](docs/architecture.md) | How a call travels through the editor, and the contract around it |
| [Glossary](docs/glossary.md) | The words the docs and the tools' answers use |

## Requirements

- Unreal Engine 5.7 or 5.8 on Windows (Win64), the only platform the plugin declares. Each release has a
  zip for each engine; a few tools need 5.8 (see the [FAQ](docs/faq.md)).
- To build from source: Visual Studio 2022 with the C++ toolchain.
- The plugin enables the engine plugins its tools use (see `UEAgent.uplugin`): among them
  PythonScriptPlugin, EditorScriptingUtilities, Niagara, PCG, StateTree, ControlRig, IKRig,
  PoseSearch, Interchange and USD. Game Features is the exception: the plugin does not turn it on,
  and its five tools work once your project enables it.

### Build from source

Copy or clone this repository to `<YourProject>/Plugins/UEAgent`, then open the project and let
Unreal build it, or build the editor target from your IDE. When the editor starts, the log shows
`MCP HTTP server started on http://127.0.0.1:55561/mcp (... tools ...)`.

## Updating

**MCP · 55561 > Check for updates** asks GitHub for the latest release. If it is newer, **Download and
install** downloads the zip for your engine, checks it against the release's `SHA256SUMS.txt`, and
installs it when the editor closes, or right away with **Restart now**, which closes the editor,
installs it and opens the editor again. The previous version is kept in `Saved/UEAgentUpdate/Backup`,
and if the new one cannot be moved in, the previous one is put back.

A git clone is updated with `git pull`, and a copy installed from Fab from the Epic Games Launcher.
After an update, reconnect your client so it lists the tools again.

## Settings

**Editor Preferences > Plugins > UEAgent** (per user):

| Setting | Default | |
|---|---|---|
| Enable HTTP server | on | the MCP server |
| HTTP port | 55561 | `-McpHttpPort=N` on the command line overrides it |
| Auth token | empty | when set, every request needs `Authorization: Bearer <token>` |
| Show connect hint | on | at startup, offer to connect Claude Code if the project is not |
| Check for updates at startup | off | ask GitHub at most once a day whether a newer release is out (see [How do I update it?](docs/faq.md#how-do-i-update-it)) |

**Project Settings > Plugins > UEAgent** (shared, saved to `Config/DefaultEditor.ini`):

| Setting | Default | |
|---|---|---|
| Read Only Content Roots | empty | vendored-library roots (e.g. `/Game/SomeMarketplacePack`): tools refuse to save, rename, move or delete anything there, and lineage audits treat their classes as library classes |
| Project Content Root | `/Game` | default scope of `audit_class_purity` |
| Fork Origin Tag | `mcp.forkOrigin` | metadata tag `fork_class_chain` writes and `audit_class_purity` checks |

## Console commands

| Command | |
|---|---|
| `mcp.http.status` / `mcp.http.restart` | server status; restart (also reloads the tool catalog) |
| `mcp.claude.status [path]` / `mcp.claude.connect [path]` | inspect / write the `.mcp.json` entry |
| `mcp.clients.status` / `mcp.clients.connect cursor\|vscode\|antigravity\|opencode [path]` | the same for the other clients' files |
| `mcp.skills.status` / `mcp.skills.install` / `mcp.skills.remove` `[claude\|agents\|all]` | UEAgent's skills in the project (see [Skills](docs/skills.md)) |
| `mcp.contract.selftest <vectors.json> [quit]` | run the contract vectors (see `Resources/ContractTests`) |
| `mcp.ReadOnlyRootEnforcement 0\|1` | 0 = only report saves under a read-only root instead of refusing them |
| `mcp.UndoStepPerCall 0\|1` | 0 = leave the undo buffer to each tool instead of one undo step per call |
| `mcp.activity [N]` | print the last N tool calls (default 20) to the log, as the Activity panel lists them |
| `mcp.update.check` / `mcp.update.status` / `mcp.update.download` / `mcp.update.cancel` | look for a newer release; the state of the update; download it (it installs when the editor closes); drop a downloaded one |

## Security

- The server listens on 127.0.0.1 only. It also refuses a request whose `Host` is not the loopback
  (DNS rebinding) or whose `Origin` is a web page.
- Without an **Auth token**, any process on the machine can call it. Set one on a shared machine;
  the `.mcp.json` entry reads it from `${UEAGENT_TOKEN}`.
- The tools act with the editor's rights: they run Python, read and write files, save, rename and
  delete assets, edit `.ini` files and launch processes (a package build, a dedicated server).
  Connecting an agent gives it the editor. **Read Only Content Roots** keeps the tools from saving,
  renaming, moving or deleting anything under the roots you list.

To report a vulnerability, see [SECURITY.md](SECURITY.md).

## Privacy

UEAgent sends nothing about you or your project: no telemetry, no analytics. Its only connection of
its own is to GitHub, to look for a newer release: when you click **Check for updates**, or once a day
on startup if you turn **Check for updates at startup** on (it is off). That request says which
version of UEAgent asks, and nothing else. The server only listens on 127.0.0.1, and a tool's answer
goes to the MCP client you connected, on the same machine. What that client then sends to its model
provider is up to the client.

## Accessibility

What the editor UI and the documentation offer for accessibility, what they lack, and how to report a
barrier: see [ACCESSIBILITY.md](ACCESSIBILITY.md).

## Contributing

Questions and ideas go to [Discussions](https://github.com/CarlxsMG/UEAgent/discussions). Bug reports
and pull requests are welcome: [CONTRIBUTING.md](CONTRIBUTING.md) explains how the code is organized,
how a tool is added and what is checked before a change is merged.

## License

UEAgent is released under the [MIT License](LICENSE).

## Trademarks

Unreal and Unreal Engine are trademarks or registered trademarks of Epic Games, Inc. in the United
States and elsewhere. UEAgent is an independent project, not affiliated with or endorsed by Epic Games.
